#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <nlohmann/json.hpp>
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
#include <string>
#include <iostream>
#include <format>

namespace BPMNOS::Rollout {

/**
 * @brief Requirements on the rollout results policy type.
 *
 * A results type is default-constructible, records a rollout's final state via `add(const SystemState*)`,
 * and is comparable with `>` so the best candidate can be selected. It reports itself two ways for progress
 * logging: `stringify()` yields a human-readable summary string, and `jsonify()` yields an
 * `nlohmann::ordered_json` entry. It is held only by `shared_ptr` and never copied, so it may carry
 * arbitrarily heavy statistics. An optional `dominates(const ResultsType&) const` lets a candidate's
 * remaining repetitions be cancelled early once the results of the greedy decision provably beat it, which
 * also excludes it from the choice.
 */
template <typename T>
concept ResultsPolicy =
  std::default_initializable<T> &&
  requires(T results, const T& a, const T& b, const BPMNOS::Execution::SystemState* state) {
    results.add(state);
    { a > b } -> std::convertible_to<bool>;
    { a.stringify() } -> std::convertible_to<std::string>;
    { a.jsonify() } -> std::convertible_to<nlohmann::ordered_json>;
  };

// The dispatcher holds a back-pointer to its owning controller and reads config / thread pool / the cut-off
// decision and advances the progress reported through it. The full definition is available at instantiation (RolloutController.h
// includes this header, defines the controller, and befriends the dispatcher).
template <ResultsPolicy ResultsType>
class RolloutController;

/**
 * @brief Generic rollout policy dispatcher owning a concrete candidate collection.
 *
 * On each dispatch the RolloutDispatcher collects the top feasible candidate decisions in reward order, the
 * front of which is the greedy decision. Every candidate, the front included, is rolled out `repetitions`
 * times from the current state on its own queue of the shared thread pool, the r-th rollout of every
 * candidate on the fork with index r, so that all candidates are valued on the same predictions; the front is
 * rolled out first, so that an alternative can be cancelled against its complete results. The candidate with
 * the best results is dispatched (ties keep the greedy front). A stored result valuing the greedy decision
 * without rolling it out would be correct only if nothing uncertain had been disclosed since it was
 * computed. When a non-greedy candidate wins, the progress reported is advanced to its results. Header-only so the results type stays an open policy
 * parameter; the forward simulation of a single candidate is delegated to the non-template Rollout. The
 * results are held by `shared_ptr` and moved, never copied, so a heavy results type is never deep-copied.
 */
template <BPMNOS::Execution::CandidateCollection Candidates, ResultsPolicy ResultsType>
class RolloutDispatcher : public BPMNOS::Execution::EventDispatcher {
public:
  /// Further arguments are forwarded to the constructor of the candidates after the evaluator.
  template <typename... Args>
  RolloutDispatcher( std::shared_ptr<BPMNOS::Execution::Evaluator> evaluator, RolloutController<ResultsType>* controller, Args&&... args )
    : candidates(evaluator, std::forward<Args>(args)...)
    , evaluator(std::move(evaluator))
    , controller(controller)
  {
    if ( controller->config.repetitions == 0 ) {
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
      if ( controller->config.candidates && decisions.size() >= controller->config.candidates ) {
        break;
      }
    }

    if ( decisions.empty() ) {
      return nullptr;
    }

    // Once the controller's cut-off is reached, stop branching and dispatch the greedy front (the
    // reward-best feasible candidate) without rolling out — exactly as a GreedyDispatcher would.
    if ( controller->cutoff() ) {
      return decisions.front();
    }

    // Every candidate, the reward-order front (the greedy decision) included, gets its own results, rolled out
    // `repetitions` times on its own queue from the current state, folding each rollout's final state in under
    // a mutex, so that all candidates are valued on the same predictions. The front is rolled out first. Once
    // its results provably dominate an alternative (if the results type supports it), that alternative's
    // remaining repetitions are cancelled by clearing its queue.
    std::vector< std::shared_ptr<ResultsType> > results( decisions.size() );
    for ( std::size_t decisionIndex = 0; decisionIndex < results.size(); ++decisionIndex ) {
      results[decisionIndex] = std::make_shared<ResultsType>();
    }
    std::vector<std::mutex> resultMutexes( decisions.size() );
    // Whether a candidate was cancelled as dominated, which excludes it from the choice whatever the mean of
    // the runs it did record. A char per candidate rather than a vector<bool>, whose flags share words and so
    // could not be written under the mutexes of different candidates.
    std::vector<char> dominated( decisions.size(), false );
    while ( queues.size() < decisions.size() ) {
      queues.push_back( controller->threadPool.addQueue() );   // one queue per candidate, reused across dispatches
    }
    std::vector< std::future<void> > jobs;
    for ( std::size_t decisionIndex = 0; decisionIndex < decisions.size(); ++decisionIndex ) {
      if ( decisionIndex == 1 ) {
        // the results of the front are complete before an alternative can be cancelled against them
        for ( auto& job : jobs ) {
          job.get();
        }
        jobs.clear();
      }
      for ( unsigned int round = 0; round < controller->config.repetitions; ++round ) {
        jobs.push_back( controller->threadPool.submit( queues[decisionIndex], [this, &decisions, &results, &resultMutexes, &dominated, systemState, decisionIndex, round]() {
          Rollout rollout( decisions[decisionIndex], systemState, evaluator, controller->config.bisect, round, copyMutex );
          if ( auto* finalState = rollout.getSystemState() ) {
            std::lock_guard lock( resultMutexes[decisionIndex] );
            results[decisionIndex]->add( finalState );
            // Cancel this alternative's remaining repetitions once the front provably dominates it.
            // Skipped when repetitions == 1: there are no remaining reps to cancel, so the early-stopping
            // test (and its overhead) is never incurred.
            if constexpr ( requires ( const ResultsType& a, const ResultsType& b ) { { a.dominates(b) } -> std::convertible_to<bool>; } ) {
              if ( decisionIndex != 0 && controller->config.repetitions > 1 && results[0]->dominates( *results[decisionIndex] ) ) {
                dominated[decisionIndex] = true;
                controller->threadPool.clearQueue( queues[decisionIndex] );
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
      controller->threadPool.clearQueue( queue );
    }

    // Dispatch the candidate with the best results, an alternative cancelled as dominated being excluded;
    // ties keep the greedy front (first maximum).
    std::size_t bestIndex = 0;
    for ( std::size_t decisionIndex = 1; decisionIndex < decisions.size(); ++decisionIndex ) {
      if ( dominated[decisionIndex] ) {
        continue;
      }
      if ( *results[decisionIndex] > *results[bestIndex] ) {
        bestIndex = decisionIndex;
      }
    }

    // A non-greedy candidate won: the progress reported is advanced (moved) to its results. No later decision
    // reads them, every candidate being rolled out afresh at every dispatch.
    if ( bestIndex != 0 ) {
      controller->baselineResults = std::move( results[bestIndex] );
      // A progress update is reported by injecting a "rollout" entry into the controller's logger when one is
      // attached; otherwise a tab-separated progress line is printed. dispatchEvent runs serially on the main
      // engine, so emitting here is not racy.
      if ( controller->logger ) {
        controller->logger->inject("rollout", nlohmann::ordered_json{ {"results", controller->baselineResults->jsonify()}, {"time", controller->elapsedSeconds()} });
      }
      else {
        std::cout << std::format("{:.1f}\t{}\n", controller->elapsedSeconds(), controller->baselineResults->stringify());
      }
    }
    return decisions[bestIndex];
  }

  void connect( BPMNOS::Execution::Mediator* mediator ) override {
    candidates.connect(mediator);   // the candidates register for the observables they need
    BPMNOS::Execution::EventDispatcher::connect(mediator);
  }

protected:
  Candidates candidates;            ///< the reward-ordered candidate collection rolled out
  std::shared_ptr<BPMNOS::Execution::Evaluator> evaluator;  ///< shared with the candidate source and with each Rollout
  RolloutController<ResultsType>* controller;   ///< owning controller; source of config (candidates/repetitions) and threadPool, holder of baselineResults, and the cut-off decision
  std::vector<ThreadPool::QueueId> queues;   ///< one queue per candidate, grown lazily and reused across dispatches
  std::mutex copyMutex;             ///< serializes each rollout's deep copy of the shared source state (the engine is not internally synchronized; the copy reads lazily-pruning containers that mutate on read)
};

} // namespace BPMNOS::Rollout
