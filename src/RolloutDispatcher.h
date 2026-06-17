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
#include <algorithm>
#include <cstddef>

namespace BPMNOS::Rollout {

/**
 * @brief Generic rollout policy dispatcher owning a concrete candidate source.
 *
 * On each dispatch the RolloutDispatcher collects the top feasible candidate decisions. The
 * reward-order front is the greedy decision: it is not rolled out — its result is the provided baseline
 * (the value of the trajectory committed so far). Each remaining candidate is rolled out `repetitions`
 * times on the shared thread pool, folding each rollout's final state into its results. The candidate with
 * the best results is dispatched (ties keep the greedy front). Header-only so the results type stays an
 * open policy parameter; the forward simulation of a single candidate is delegated to the non-template
 * Rollout.
 */
template <typename Source, typename ResultsType>
class RolloutDispatcher : public BPMNOS::Execution::EventDispatcher {
public:
  RolloutDispatcher( BPMNOS::Execution::Evaluator* evaluator, const ResultsType& baseline, unsigned int candidates, unsigned int repetitions, ThreadPool& threadPool )
    : source(evaluator)
    , evaluator(evaluator)
    , baseline(baseline)
    , candidates(candidates)
    , repetitions(repetitions)
    , threadPool(threadPool)
    , queue(threadPool.addQueue())
  {
  }

  std::shared_ptr<BPMNOS::Execution::Event> dispatchEvent( const BPMNOS::Execution::SystemState* systemState ) override {
    // Collect the top-`candidates` feasible candidate decisions in reward order (0 = all).
    std::vector< std::shared_ptr<BPMNOS::Execution::Decision> > decisions;
    for ( auto candidate : source.getCandidates(systemState) ) {
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
      if ( candidates && decisions.size() >= candidates ) {
        break;
      }
    }

    if ( decisions.empty() ) {
      return nullptr;
    }

    // The reward-order front is the greedy decision: it is not rolled out — its result is the provided
    // baseline. Each remaining candidate is rolled out `repetitions` times on the thread pool, folding
    // each rollout's final state into its results (skipping a null/placeholder state); then wait.
    std::vector<ResultsType> results( decisions.size() );
    results[0] = baseline;
    std::vector<std::mutex> resultMutexes( decisions.size() );
    std::vector< std::future<void> > jobs;
    unsigned int rounds = std::max(1u, repetitions);
    for ( std::size_t i = 1; i < decisions.size(); ++i ) {
      for ( unsigned int repetition = 0; repetition < rounds; ++repetition ) {
        jobs.push_back( threadPool.submit( queue, [this, &decisions, &results, &resultMutexes, systemState, i]() {
          Rollout rollout( decisions[i], systemState, evaluator );
          if ( auto* finalState = rollout.getSystemState() ) {
            std::lock_guard lock( resultMutexes[i] );
            results[i].add( finalState );
          }
        }) );
      }
    }
    for ( auto& job : jobs ) {
      job.get();
    }

    // Dispatch the candidate with the best results; ties keep the greedy front (first maximum).
    std::size_t best = 0;
    for ( std::size_t i = 1; i < decisions.size(); ++i ) {
      if ( results[i] > results[best] ) {
        best = i;
      }
    }
    return decisions[best];
  }

  void connect( BPMNOS::Execution::Mediator* mediator ) override {
    source.connect(mediator);   // a stateful source subscribes to its request type and DataUpdate
    BPMNOS::Execution::EventDispatcher::connect(mediator);
  }

protected:
  Source source;
  BPMNOS::Execution::Evaluator* evaluator;
  const ResultsType& baseline;   ///< Value of the trajectory committed so far; the greedy front uses it instead of being re-rolled.
  unsigned int candidates;       ///< max candidates assessed per dispatch (0 = all)
  unsigned int repetitions;      ///< rollouts per non-greedy candidate
  ThreadPool& threadPool;        ///< shared pool the rollouts run on
  ThreadPool::QueueId queue;     ///< this dispatcher's queue in the pool (reused each dispatch)
};

} // namespace BPMNOS::Rollout
