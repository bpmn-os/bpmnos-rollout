#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include "Rollout.h"
#include "ThreadPool.h"
#include <memory>
#include <limits>
#include <tuple>
#include <vector>
#include <mutex>
#include <future>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <concepts>

namespace BPMNOS::Rollout {

/**
 * @brief Requirements on the rollout results policy type.
 *
 * A results type is default-constructible, records a rollout's final state via `add(const SystemState*)`,
 * and is comparable with `>` so the best candidate can be selected. It is held only by `shared_ptr` and
 * never copied, so it may carry arbitrarily heavy statistics. An optional `dominates(const ResultsType&)
 * const` lets a candidate's remaining repetitions be cancelled early once the baseline provably beats it.
 */
template <typename T>
concept ResultsPolicy =
  std::default_initializable<T> &&
  requires(T results, const T& a, const T& b, const BPMNOS::Execution::SystemState* state) {
    results.add(state);
    { a > b } -> std::convertible_to<bool>;
  };

/**
 * @brief Generic rollout policy dispatcher owning a concrete candidate collection.
 *
 * On each dispatch the RolloutDispatcher collects the top feasible candidate decisions in reward order. The
 * reward-order front is the greedy decision: it is not rolled out — its result is the shared baseline (the
 * value of the trajectory committed so far). Each remaining candidate is rolled out `repetitions` times on
 * its own queue of the shared thread pool, folding each rollout's final state into its own results. The
 * candidate with the best results is dispatched (ties keep the greedy front), and when a non-greedy candidate
 * wins the baseline is advanced to its results. Header-only so the results type stays an open policy
 * parameter; the forward simulation of a single candidate is delegated to the non-template Rollout. The
 * results are held by `shared_ptr` and moved, never copied, so a heavy results type is never deep-copied.
 */
template <BPMNOS::Execution::CandidateCollection Candidates, ResultsPolicy ResultsType>
class RolloutDispatcher : public BPMNOS::Execution::EventDispatcher {
public:
  RolloutDispatcher( BPMNOS::Execution::Evaluator* evaluator, std::shared_ptr<ResultsType>& baseline, unsigned int maxCandidates, unsigned int repetitions, ThreadPool& threadPool )
    : candidates(evaluator)
    , evaluator(evaluator)
    , baseline(baseline)
    , maxCandidates(maxCandidates)
    , repetitions(repetitions)
    , threadPool(threadPool)
  {
    if ( repetitions == 0 ) {
      throw std::invalid_argument("RolloutDispatcher: repetitions must be at least 1");
    }
  }

  std::shared_ptr<BPMNOS::Execution::Event> dispatchEvent( const BPMNOS::Execution::SystemState* systemState ) override {
    // Collect the top-`maxCandidates` feasible candidate decisions in reward order (0 = all).
    std::vector< std::shared_ptr<BPMNOS::Execution::Decision> > decisions;
    for ( auto candidate : candidates ) {
      if ( std::get<0>(candidate) <= -std::numeric_limits<double>::infinity() ) {
        break; // reward-ordered descending: infeasible candidates (-infinity) sort last
      }
      // The decision is the weak Event, the second-to-last element of the candidate tuple.
      constexpr std::size_t eventIndex = std::tuple_size<decltype(candidate)>::value - 2;
      auto event = std::get<eventIndex>(candidate).lock();
      if ( !event ) {
        continue; // decision expired before it could be assessed
      }
      auto decision = std::dynamic_pointer_cast<BPMNOS::Execution::Decision>(event);
      if ( !decision ) {
        continue;
      }
      decisions.push_back( std::move(decision) );
      if ( maxCandidates && decisions.size() >= maxCandidates ) {
        break;
      }
    }

    if ( decisions.empty() ) {
      return nullptr;
    }

    // The reward-order front is the greedy decision: it is not rolled out — its result is the shared
    // baseline (and is never mutated below). Each remaining candidate gets its own results, rolled out
    // `repetitions` times on its own queue, folding each rollout's final state in under a mutex. Once the
    // baseline provably dominates a candidate (if the results type supports it), that candidate's remaining
    // repetitions are cancelled by clearing its queue.
    std::vector< std::shared_ptr<ResultsType> > results( decisions.size() );
    results[0] = baseline;
    for ( std::size_t decisionIndex = 1; decisionIndex < results.size(); ++decisionIndex ) {
      results[decisionIndex] = std::make_shared<ResultsType>();
    }
    std::vector<std::mutex> resultMutexes( decisions.size() );
    while ( queues.size() < decisions.size() ) {
      queues.push_back( threadPool.addQueue() );   // one queue per candidate, reused across dispatches
    }
    std::vector< std::future<void> > jobs;
    for ( std::size_t decisionIndex = 1; decisionIndex < decisions.size(); ++decisionIndex ) {
      for ( unsigned int round = 0; round < repetitions; ++round ) {
        jobs.push_back( threadPool.submit( queues[decisionIndex], [this, &decisions, &results, &resultMutexes, systemState, decisionIndex, round]() {
          Rollout rollout( decisions[decisionIndex], systemState, evaluator, round );
          if ( auto* finalState = rollout.getSystemState() ) {
            std::lock_guard lock( resultMutexes[decisionIndex] );
            results[decisionIndex]->add( finalState );
            // Cancel this candidate's remaining repetitions once the baseline provably dominates it.
            if constexpr ( requires ( const ResultsType& a, const ResultsType& b ) { { a.dominates(b) } -> std::convertible_to<bool>; } ) {
              if ( baseline->dominates( *results[decisionIndex] ) ) {
                threadPool.clearQueue( queues[decisionIndex] );
              }
            }
          }
        }) );
      }
    }
    for ( auto& job : jobs ) {
      try {
        job.get();
      }
      catch ( const std::future_error& ) {
        // a cancelled repetition was dropped from its queue; the candidate keeps the samples it did record
      }
    }
    // Clear every queue so the next dispatch starts from a clean pool.
    for ( auto& queue : queues ) {
      threadPool.clearQueue( queue );
    }

    // Dispatch the candidate with the best results; ties keep the greedy front (first maximum).
    std::size_t bestIndex = 0;
    for ( std::size_t decisionIndex = 1; decisionIndex < decisions.size(); ++decisionIndex ) {
      if ( *results[decisionIndex] > *results[bestIndex] ) {
        bestIndex = decisionIndex;
      }
    }

    // A non-greedy candidate won: the committed trajectory becomes this candidate's rolled-out trajectory,
    // so the shared baseline is advanced (moved) to its results. Dispatching the greedy front (bestIndex 0)
    // leaves the committed trajectory — and thus the baseline — unchanged.
    if ( bestIndex != 0 ) {
      baseline = std::move( results[bestIndex] );
    }
    return decisions[bestIndex];
  }

  void connect( BPMNOS::Execution::Mediator* mediator ) override {
    candidates.connect(mediator);   // the candidates register for the observables they need
    BPMNOS::Execution::EventDispatcher::connect(mediator);
  }

protected:
  Candidates candidates;            ///< the reward-ordered candidate collection rolled out
  BPMNOS::Execution::Evaluator* evaluator;
  std::shared_ptr<ResultsType>& baseline;   ///< the controller's baselineResults: the committed trajectory's value (the greedy front shares it instead of being re-rolled); reseated to the winner's results when a non-greedy candidate is dispatched
  unsigned int maxCandidates;       ///< max candidate decisions assessed per dispatch (0 = all)
  unsigned int repetitions;         ///< rollouts per non-greedy candidate
  ThreadPool& threadPool;           ///< shared pool the rollouts run on
  std::vector<ThreadPool::QueueId> queues;   ///< one queue per candidate, grown lazily and reused across dispatches
};

} // namespace BPMNOS::Rollout
