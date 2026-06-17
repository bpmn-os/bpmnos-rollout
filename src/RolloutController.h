#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include "Results.h"
#include "ThreadPool.h"
#include "RolloutDispatcher.h"
#include <memory>
#include <vector>

namespace BPMNOS::Rollout {

/**
 * @brief Controller selecting decisions by one-step lookahead rollout.
 *
 * The rollout controller makes desicisions assuming that entry and exit decisions
 * of activities (which are not children a SequentialAdHocSubProcess) can be made immediately
 * when they are feasible, that direct messages can be delivered immediately. Only when no
 * immediate entry, exit, or direct message delivery decision is made, it starts the rollout
 * mechanism which iterates over decision candidates, forces each candidate and completes the
 * forward simulation under a greedy policy, then dispatches the candidate yielding the best
 * final objective.
 *
 * Thus, the controller makes its decisions in the following order:
 *  1. `GreedyDispatcher<FirstFeasibleExit>`  — dispatch a feasible exit immediately (exits never compete).
 *  2. `GreedyDispatcher<FirstFeasibleEntry>` — dispatch a feasible entry immediately, unless it is a
 *                         child of a SequentialAdHocSubProcess.
 *  3. `InstantDirectMessage` — dispatch an explicitly addressed message delivery immediately (the
 *                         receive specifies its sender, or the message names this recipient).
 *  4. `RolloutDispatcher<FirstBisectionalChoice>` — roll out choice requests individually.
 *  5. `RolloutDispatcher<SequentialEntries>` and `RolloutDispatcher<MessageDeliveries>` — roll out the
 *                         remaining contested groups, which have no precedence between them:
 *                         sequential-ad-hoc child entries and message deliveries.
 *
 * Templated on the results type (the aggregation/selection policy); header-only so it never assumes a
 * particular results type.
 *
 * Parameters:
 *  - `candidates`   — maximum number of options assessed per contested decision
 *                     (0 = all), pre-ranked by local evaluation.
 *  - `repetitions`  — rollouts per candidate for stochastic scenarios (averaged);
 *                     a deterministic scenario uses a single rollout. Repetitions
 *                     share the same seed set across all candidates (common random
 *                     numbers) so the comparison reflects the decision, not the draw.
 *  - `threads`      — number of threads that can be used to run independent rollouts in parallel.
 */
template <typename ResultsType>
class RolloutController : public BPMNOS::Execution::Controller {
public:
  /**
   * @brief Rollout parameters.
   */
  struct Config {
    unsigned int candidates = 0;  ///< max candidate decisions assessed per contested decision (0 = all)
    unsigned int repetitions = 1; ///< rollouts per candidate for stochastic scenarios (averaged)
    unsigned int threads = 1;     ///< number of parallel rollout threads
  };
  static Config default_config() { return {}; } // Workaround for compiler bug, as in GreedyController (a `Config config = {}` default argument fails to compile).

  RolloutController(BPMNOS::Execution::Evaluator* evaluator, const ResultsType& greedyResults, Config config = default_config())
    : config(config)
    , baselineResults(greedyResults)
    , threadPool(config.threads)
  {
    using namespace BPMNOS::Execution;
    // Prioritized layer: dispatch the first feasible decision. Entry, exit, and direct message delivery
    // never branch, so they are dispatched greedily from the same Candidates sources as GreedyController.
    prioritizedDispatchers.push_back( std::make_unique<GreedyDispatcher<FirstFeasibleExit>>(evaluator) );
    prioritizedDispatchers.push_back( std::make_unique<GreedyDispatcher<FirstFeasibleEntry>>(evaluator) ); // non-sequential entries only (config.sequential=false)
    prioritizedDispatchers.push_back( std::make_unique<InstantDirectMessage>() );
//    prioritizedDispatchers.push_back( std::make_unique<RolloutDispatcher<FirstEnumeratedChoice, ResultsType>>(evaluator, baselineResults, config.candidates, config.repetitions, threadPool) );
    prioritizedDispatchers.push_back( std::make_unique<RolloutDispatcher<FirstBisectionalChoice, ResultsType>>(evaluator, baselineResults, config.candidates, config.repetitions, threadPool) );
    // Competing layer: best-of-best over the contested decisions.
    competingDispatchers.push_back( std::make_unique<RolloutDispatcher<SequentialEntries, ResultsType>>(evaluator, baselineResults, config.candidates, config.repetitions, threadPool) ); // sequential ad-hoc entries only
    competingDispatchers.push_back( std::make_unique<RolloutDispatcher<MessageDeliveries, ResultsType>>(evaluator, baselineResults, config.candidates, config.repetitions, threadPool) );
  }

  void connect(BPMNOS::Execution::Mediator* mediator) override {
    for ( auto& eventDispatcher : prioritizedDispatchers ) {
      eventDispatcher->connect(this);
    }
    for ( auto& eventDispatcher : competingDispatchers ) {
      eventDispatcher->connect(this);
    }
    BPMNOS::Execution::Controller::connect(mediator);
  }

protected:
  std::shared_ptr<BPMNOS::Execution::Event> dispatchEvent(const BPMNOS::Execution::SystemState* systemState) override {
    using namespace BPMNOS::Execution;
    // Instant layer: dispatch the first feasible decision in priority order.
    for ( auto& eventDispatcher : prioritizedDispatchers ) {
      if ( auto event = eventDispatcher->dispatchEvent(systemState) ) {
        if ( auto decision = dynamic_pointer_cast<Decision>(event) ) {
          if ( decision->reward().has_value() ) {
            return event;
          }
        }
        else {
          // events are immediately forwarded
          return event;
        }
      }
    }

    // Competing layer: dispatch the best evaluated decision (best-of-best).
    std::shared_ptr<Decision> best = nullptr;
    for ( auto& eventDispatcher : competingDispatchers ) {
      if ( auto event = eventDispatcher->dispatchEvent(systemState) ) {
        if ( auto decision = dynamic_pointer_cast<Decision>(event) ) {
          if ( decision->reward().has_value() ) {
            if ( !best ) {
              // first feasible decision is used as best
              best = decision;
            }
            else if ( decision->reward().value() > best->reward().value() ) {
              // decision has better reward than current best
              best = decision;
            }
          }
        }
        else {
          // events are immediately forwarded
          return event;
        }
      }
    }

    return best;
  }

private:
  Config config;
  ResultsType baselineResults;   ///< Baseline the rollout compares against; starts as the greedy result, updated to the winner's result whenever a non-greedy candidate is dispatched.
  ThreadPool threadPool;         ///< Shared pool the rollout dispatchers run their rollouts on (sized config.threads).
  std::vector< std::unique_ptr<BPMNOS::Execution::EventDispatcher> > prioritizedDispatchers;   ///< Dispatched first-feasible in priority order.
  std::vector< std::unique_ptr<BPMNOS::Execution::EventDispatcher> > competingDispatchers;     ///< Dispatched by best-of-best rolled-out objective.
};

} // namespace BPMNOS::Rollout
