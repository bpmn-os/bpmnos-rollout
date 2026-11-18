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
#include <unordered_set>
#include <algorithm>
#include <cassert>
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
 * arbitrarily heavy statistics. An optional `dominates(const ResultsType&) const`, together with the recorded
 * objectives `objectives`, by whose number the dispatcher tells when the results of the greedy decision are
 * complete, lets a candidate's remaining repetitions be cancelled early once the results of the greedy decision
 * provably beat it, which also excludes it from the choice.
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
 * @brief Rollout policy dispatcher valuing the candidates and the deferrals of the decision points by simulation.
 *
 * The dispatcher owns the collections of candidates of the choices, of the entries of children of sequential ad-hoc
 * subprocesses and of the deliveries of messages, constructed with the deferrals of the controller, so that the
 * candidates of deferred decision points are neither evaluated nor yielded. On each dispatch it takes up to `candidates`
 * feasible candidates from each collection, all if `candidates` is zero, and sorts them by their rewards, a tie keeping
 * the order of the collections, the front being the greedy decision. The options are these candidates in this order,
 * the deferral of the decision point of a candidate being inserted immediately after the first, best candidate of the
 * decision point if the model states a deferral there whose condition holds on the token of the decision point and if
 * the deferral is admissible. A deferral thus is an option once per decision point, and the first `candidates` options
 * are rolled out, the deferrals counting towards them.
 *
 * Every option is rolled out `repetitions` times from the current state on its own queue of the shared thread pool,
 * the r-th rollout of every option on the fork with index r, so that all options are valued on the same predictions,
 * with the decision points deferred by the controller deferred in the simulation as well. With a single repetition the
 * front is valued by the stored baseline. This assumes that a scenario whose future is uncertain is always run with
 * several repetitions, so that with a single one nothing uncertain is disclosed and the baseline is exactly the rollout
 * of the front. With several repetitions the front is rolled out as every other option and together with them, the
 * baseline having drawn values the run may since have disclosed otherwise, and an option is cancelled against the
 * results of the front once they are complete. The option with the best results is taken, a tie keeping the earlier
 * option and a deferral never winning a tie against acting. When an option other than the front wins, the baseline is
 * advanced to its results. A candidate taken is dispatched; a deferral taken is placed in the deferrals of the
 * controller, after which the dispatcher chooses again among the options remaining, and it returns no event only when
 * none remains. Header-only so the results type stays an open policy parameter; the forward simulation of a single
 * option is delegated to the non-template Rollout. The results are held by `shared_ptr` and moved, never copied, so a
 * heavy results type is never deep-copied.
 */
template <ResultsPolicy ResultsType>
class RolloutDispatcher : public BPMNOS::Execution::EventDispatcher {
public:
  RolloutDispatcher( std::shared_ptr<BPMNOS::Execution::Evaluator> evaluator, RolloutController<ResultsType>* controller )
    : choices(evaluator, controller->deferrals, controller->config.bisect)
    , entries(evaluator, controller->deferrals)
    , deliveries(evaluator, controller->deferrals)
    , evaluator(std::move(evaluator))
    , controller(controller)
  {
    if ( controller->config.repetitions == 0 ) {
      throw std::invalid_argument("RolloutDispatcher: repetitions must be at least 1");
    }
  }

  std::shared_ptr<BPMNOS::Execution::Event> dispatchEvent( const BPMNOS::Execution::SystemState* systemState ) override {
    // a deferral taken is placed and the options remaining are valued anew with it in place
    for (;;) {
      auto options = gatherOptions();
      if ( options.empty() ) {
        return nullptr;
      }

      // Once the controller's cut-off is reached, stop branching and dispatch the greedy front (the
      // reward-best feasible candidate) without rolling out — exactly as a greedy controller would.
      if ( controller->cutoff() ) {
        return options.front().decision;
      }

      auto best = rollOut(options, systemState);
      if ( !options[best].deferral ) {
        return options[best].decision;
      }
      auto token = options[best].decision->token.lock();
      assert( token );
      controller->deferrals.defer(token.get());
    }
  }

  void connect( BPMNOS::Execution::Mediator* mediator ) override {
    // the candidates register for the observables they need
    choices.connect(mediator);
    entries.connect(mediator);
    deliveries.connect(mediator);
    BPMNOS::Execution::EventDispatcher::connect(mediator);
  }

protected:
  /// An option rolled out: a candidate decision, or the deferral of the decision point of the candidate decision.
  struct Option {
    std::shared_ptr<BPMNOS::Execution::Decision> decision;
    bool deferral;
  };

  BPMNOS::Execution::Choices choices;                       ///< the candidates of the choices
  BPMNOS::Execution::SequentialEntries entries;             ///< the candidates of the entries of children of sequential ad-hoc subprocesses
  BPMNOS::Execution::MessageDeliveries deliveries;          ///< the candidates of the deliveries of messages that are not explicitly addressed
  std::shared_ptr<BPMNOS::Execution::Evaluator> evaluator;  ///< shared with the candidate collections and with each Rollout
  RolloutController<ResultsType>* controller;   ///< owning controller; source of config (candidates/repetitions), threadPool and deferrals, holder of baselineResults, and the cut-off decision
  std::vector<ThreadPool::QueueId> queues;   ///< one queue per option, grown lazily and reused across dispatches
  std::mutex copyMutex;             ///< serializes each rollout's deep copy of the shared source state (the engine is not internally synchronized; the copy reads lazily-pruning containers that mutate on read)

  /// Appends up to `candidates` feasible candidates of the collection, all if `candidates` is zero, with their rewards.
  template <typename Collection>
  void gather( Collection& collection, std::vector< std::pair< double, std::shared_ptr<BPMNOS::Execution::Decision> > >& candidates ) {
    std::size_t count = 0;
    for ( auto candidate : collection ) {
      if ( std::get<0>(candidate) <= -std::numeric_limits<double>::infinity() ) {
        break; // reward-ordered descending: infeasible candidates (-infinity) sort last
      }
      // The decision is the weak Event, the second-to-last element of the candidate tuple.
      constexpr std::size_t eventIndex = std::tuple_size<decltype(candidate)>::value - 2;
      auto decision = std::dynamic_pointer_cast<BPMNOS::Execution::Decision>( std::get<eventIndex>(candidate).lock() );
      if ( !decision ) {
        continue; // decision expired before it could be assessed
      }
      candidates.emplace_back( std::get<0>(candidate), std::move(decision) );
      if ( controller->config.candidates && ++count >= controller->config.candidates ) {
        break;
      }
    }
  }

  /// Returns the first `candidates` options, all if `candidates` is zero: the candidates of all collections by their
  /// rewards, the deferral of a decision point following its first candidate where it may be taken.
  std::vector<Option> gatherOptions() {
    using namespace BPMNOS::Execution;
    std::vector< std::pair< double, std::shared_ptr<Decision> > > candidates;
    gather(choices, candidates);
    gather(entries, candidates);
    gather(deliveries, candidates);
    // a tie keeps the order of the collections, as in the greedy controller
    std::ranges::stable_sort(candidates, std::ranges::greater{}, [](const auto& candidate) { return candidate.first; });

    const std::size_t limit = controller->config.candidates;
    std::vector<Option> options;
    std::unordered_set<const Token*> decisionPoints;
    for ( auto& [reward, decision] : candidates ) {
      if ( limit && options.size() >= limit ) {
        break;
      }
      options.push_back( { decision, false } );
      auto token = decision->token.lock();
      assert( token );
      auto decisionPoint = Deferrals::decisionPoint(token.get());
      if ( !decisionPoints.insert(decisionPoint).second ) {
        continue; // the deferral of the decision point follows its first candidate
      }
      auto deferral = Deferrals::deferralOf(decisionPoint);
      if (
        deferral &&
        ( !limit || options.size() < limit ) &&
        deferral->conditionHolds(decisionPoint->status, *decisionPoint->data) &&
        controller->deferrals.isAdmissible(deferral)
      ) {
        options.push_back( { decision, true } );
      }
    }
    return options;
  }

  /// Rolls out the options and returns the index of the option taken, advancing the baseline if it is not the front.
  std::size_t rollOut( const std::vector<Option>& options, const BPMNOS::Execution::SystemState* systemState ) {
    // The decision points deferred by the controller are deferred in every simulation as well; each is identified by
    // the token of one of its requests, which the simulation finds in its copy of the state.
    std::vector<const BPMNOS::Execution::Token*> deferredTokens;
    for ( auto& [decisionPoint, entry] : controller->deferrals.getDeferrals() ) {
      for ( auto& request_ptr : std::get<0>(entry) ) {
        if ( auto request = request_ptr.lock() ) {
          deferredTokens.push_back( request->token );
          break;
        }
      }
    }

    // Every option gets its own results, rolled out `repetitions` times on its own queue from the current state,
    // folding each rollout's final state in under a mutex, so that all options are valued on the same predictions.
    // With a single repetition the stored baseline values the reward-order front (the greedy decision). This assumes
    // that a scenario whose future is uncertain is always run with several repetitions, so that with a single one
    // nothing is disclosed that the baseline did not contain and the baseline equals the rollout of the front. With
    // several repetitions the front is rolled out as every other option, since the baseline drew values the run may
    // since have disclosed otherwise. All options are rolled out together, so that no thread waits; once the results of
    // the front are complete and provably dominate an option (if the results type supports it), that option's
    // remaining repetitions are cancelled by clearing its queue.
    const bool noRepetitions = controller->config.repetitions == 1;
    std::vector< std::shared_ptr<ResultsType> > results( options.size() );
    for ( std::size_t optionIndex = 0; optionIndex < results.size(); ++optionIndex ) {
      results[optionIndex] = std::make_shared<ResultsType>();
    }
    if ( noRepetitions ) {
      results[0] = controller->baselineResults;
    }
    std::vector<std::mutex> resultMutexes( options.size() );
    // Whether an option was cancelled as dominated, which excludes it from the choice whatever the mean of the runs it
    // did record. A char per option rather than a vector<bool>, whose flags share words and so could not be written
    // under the mutexes of different options.
    std::vector<char> dominated( options.size(), false );
    while ( queues.size() < options.size() ) {
      queues.push_back( controller->threadPool.addQueue() );   // one queue per option, reused across dispatches
    }
    std::vector< std::future<void> > jobs;
    for ( std::size_t optionIndex = noRepetitions ? 1 : 0; optionIndex < options.size(); ++optionIndex ) {
      for ( unsigned int round = 0; round < controller->config.repetitions; ++round ) {
        jobs.push_back( controller->threadPool.submit( queues[optionIndex], [this, &options, &deferredTokens, &results, &resultMutexes, &dominated, systemState, optionIndex, round]() {
          Rollout rollout( options[optionIndex].decision, options[optionIndex].deferral, deferredTokens, systemState, evaluator, controller->config.bisect, round, copyMutex );
          if ( auto* finalState = rollout.getSystemState() ) {
            std::lock_guard lock( resultMutexes[optionIndex] );
            results[optionIndex]->add( finalState );
            // Cancel this option's remaining repetitions once the results of the front are complete and provably
            // dominate it. Skipped when repetitions == 1: there are no remaining reps to cancel, so the early-stopping
            // test (and its overhead) is never incurred. The mutex of the front is taken while holding that of the
            // option, never the other way round.
            if constexpr ( requires ( const ResultsType& a, const ResultsType& b ) { { a.dominates(b) } -> std::convertible_to<bool>; { a.objectives.size() } -> std::convertible_to<std::size_t>; } ) {
              if ( optionIndex != 0 && controller->config.repetitions > 1 ) {
                std::lock_guard frontLock( resultMutexes[0] );
                if ( results[0]->objectives.size() == controller->config.repetitions && results[0]->dominates( *results[optionIndex] ) ) {
                  dominated[optionIndex] = true;
                  controller->threadPool.clearQueue( queues[optionIndex] );
                }
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
        // a cancelled repetition was dropped from its queue; the option keeps the samples it did record
      }
    }
    // Clear every queue so the next dispatch starts from a clean pool.
    for ( auto& queue : queues ) {
      controller->threadPool.clearQueue( queue );
    }

    // Take the option with the best results, an option cancelled as dominated being excluded; a tie keeps the earlier
    // option, except that acting wins a tie against a deferral, so that rollout never waits without reason.
    std::size_t bestIndex = 0;
    for ( std::size_t optionIndex = 1; optionIndex < options.size(); ++optionIndex ) {
      if ( dominated[optionIndex] ) {
        continue;
      }
      if (
        *results[optionIndex] > *results[bestIndex] ||
        ( options[bestIndex].deferral && !options[optionIndex].deferral && !( *results[bestIndex] > *results[optionIndex] ) )
      ) {
        bestIndex = optionIndex;
      }
    }

    // An option other than the front won: the baseline is advanced (moved) to its results, which value the front at
    // the next dispatch with a single repetition and are reported as progress in either case. After a deferral, the
    // front of the next dispatch is the decision greedy takes with the deferral in place, which its results value.
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
    return bestIndex;
  }
};

} // namespace BPMNOS::Rollout
