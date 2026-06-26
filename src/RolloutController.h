#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include "Results.h"
#include "ThreadPool.h"
#include "RolloutDispatcher.h"
#include "TwoStepRolloutDispatcher.h"
#include "DecisionCounter.h"
#include <memory>
#include <utility>
#include <vector>
#include <chrono>
#include <iostream>
#include <format>
#include <stdexcept>

namespace BPMNOS::Rollout {

/**
 * @brief Controller selecting decisions by one-step lookahead rollout.
 *
 * The rollout controller assumes that entry and exit decisions of activities (which are not children of a
 * SequentialAdHocSubProcess) can be made immediately when they are feasible, and that explicitly addressed
 * messages can be delivered immediately. Only when no such immediate decision is made does it start the
 * rollout mechanism, which iteratively applies candidate decisions completing the forward simulation
 * under a greedy policy. It dispatches the candidate yielding the best final objective.
 *
 * Thus, the controller makes its decisions in the following order, dispatching the first feasible one:
 *  1. `GreedyDispatcher<FirstFeasibleExit>`  — dispatch a feasible exit immediately (exits never compete).
 *  2. `GreedyDispatcher<FirstFeasibleEntry>` — dispatch a feasible entry immediately, unless it is a
 *                         child of a SequentialAdHocSubProcess.
 *  3. `InstantDirectMessage` — dispatch an explicitly addressed message delivery immediately (the
 *                         receive specifies its sender, or the message names this recipient).
 *  4. `RolloutDispatcher<FirstEnumeratedChoice>` — roll out the alternatives of a choice request.
 *  5. `RolloutDispatcher<CompetingCandidates>` — roll out the remaining contested decisions, which have no
 *                         precedence between them: sequential-ad-hoc child entries and message deliveries,
 *                         merged into a single reward-ordered collection.
 *
 * Templated on the results type (the aggregation/selection policy); header-only so it never assumes a
 * particular results type. The baseline against which rollouts are compared is held by `shared_ptr` and
 * advanced (moved) to the winner's results, so a heavy results type is never deep-copied.
 */
template <ResultsPolicy ResultsType>
class RolloutController : public BPMNOS::Execution::Controller {
public:
  /**
   * @brief Rollout parameters.
   */
  struct Config {
    unsigned int candidates = 0;  ///< max candidate decisions to be rolled out (0 = unlimited)
    unsigned int repetitions = 1; ///< rollouts per candidate for stochastic scenarios (averaged)
    unsigned int cutoff = 0;      ///< max number of decisions made before switching to greedy (0 = unlimited)
    unsigned int lookahead = 1;   ///< lookahead depth: 1 = one-step rollout (default), 2 = two-step rollout (deeper not supported).
    unsigned int threads = 1;     ///< number of threads used for rollouts (0 = all available hardware threads)
    bool bisection = false;       ///< If true, use FirstBisectionalChoice, otherwise use FirstEnumeratedChoice (one-step lookahead only).
  };
  static Config default_config() { return {}; } // Workaround for compiler bug, as in GreedyController (a `Config config = {}` default argument fails to compile).

  RolloutController(BPMNOS::Execution::Evaluator* evaluator, std::shared_ptr<ResultsType> greedyResults, Config config = default_config(), std::unique_ptr<BPMNOS::Execution::Recorder> logger = nullptr)
    : config(config)
    , baselineResults(std::move(greedyResults))
    , logger(std::move(logger))
    , threadPool(config.threads)
  {
    using namespace BPMNOS::Execution;
    // Dispatch the first feasible decision in priority order. Exit, entry, and direct message delivery never
    // branch, so they are dispatched greedily from the same Candidates collections as GreedyController; the
    // contested choice, and the merged sequential entries and message deliveries, are rolled out. All rollout
    // dispatchers share the same baseline, advanced in place to a winner's results.
    dispatchers.push_back( std::make_unique<GreedyDispatcher<FirstFeasibleExit>>(evaluator) );
    dispatchers.push_back( std::make_unique<GreedyDispatcher<FirstFeasibleEntry>>(evaluator) ); // non-sequential entries only (config.sequential=false)
    dispatchers.push_back( std::make_unique<InstantDirectMessage>() );
    if ( config.lookahead == 1 ) {
      if ( config.bisection ) {
        dispatchers.push_back( std::make_unique<RolloutDispatcher<FirstBisectionalChoice, ResultsType>>(evaluator, this) );
      }
      else {
        dispatchers.push_back( std::make_unique<RolloutDispatcher<FirstEnumeratedChoice, ResultsType>>(evaluator, this) );
      }
      dispatchers.push_back( std::make_unique<RolloutDispatcher<CompetingCandidates, ResultsType>>(evaluator, this) ); // sequential ad-hoc entries and message deliveries
    }
    else if ( config.lookahead == 2 ) {
      // Two-step lookahead: a single dispatcher handles both contested kinds (choice and competing), branching
      // the first contested decision and the next one reached before falling back to the greedy policy.
      if ( config.bisection ) {
        throw std::invalid_argument("RolloutController: bisection is only supported with lookahead == 1");
      }
      dispatchers.push_back( std::make_unique<TwoStepRolloutDispatcher<ResultsType>>(evaluator, this) );
    }
    else {
      throw std::invalid_argument("RolloutController: lookahead must be 1 or 2");
    }

    // Report the initial baseline: inject a "rollout" entry into the logger when one was provided, otherwise
    // print the tab-separated progress header and first row.
    if ( this->logger ) {
      this->logger->inject("rollout", nlohmann::ordered_json{ {"results", baselineResults->jsonify()}, {"time", 0.0} });
    }
    else {
      std::cout << "\nTime\tBestResults\n";
      std::cout << std::format("0.0\t{}\n", baselineResults->stringify());
    }
  }

  void connect(BPMNOS::Execution::Mediator* mediator) override {
    for ( auto& eventDispatcher : dispatchers ) {
      eventDispatcher->connect(this);
    }
    if ( config.cutoff ) {
      decisionCounter.connect(mediator);   // count rolled-out decisions only when a cutoff is set (no overhead otherwise)
    }
    BPMNOS::Execution::Controller::connect(mediator);
  }

  /// True once the number of rolled-out decisions dispatched on the main engine reaches the cutoff; the
  /// rollout dispatchers then fall back to greedy. `config.cutoff == 0` means no cutoff (never true).
  bool cutoff() const {
    return config.cutoff && decisionCounter.count() >= config.cutoff;
  }

  /// Wall-clock seconds elapsed since this controller was constructed (≈ since the live run started);
  /// read by the rollout dispatchers to report when each baseline update landed.
  double elapsedSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
  }

protected:
  std::shared_ptr<BPMNOS::Execution::Event> dispatchEvent(const BPMNOS::Execution::SystemState* systemState) override {
    using namespace BPMNOS::Execution;
    // Dispatch the first feasible decision in priority order; forward any non-decision event immediately.
    for ( auto& eventDispatcher : dispatchers ) {
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

    return nullptr;
  }

private:
  // The rollout dispatchers hold a back-pointer to this controller and read config, threadPool,
  // baselineResults, and cutoff() through it.
  template <BPMNOS::Execution::CandidateCollection C, ResultsPolicy R>
  friend class RolloutDispatcher;
  template <ResultsPolicy R>
  friend class TwoStepRolloutDispatcher;

  Config config;
  std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();   ///< construction time; anchors elapsedSeconds() (≈ start of the live run, as the controller is built just before engine.run).
  std::shared_ptr<ResultsType> baselineResults;   ///< Baseline the rollout compares against; starts as the greedy result, reseated to the winner's result whenever a non-greedy candidate is dispatched. Read (and advanced) by every rollout dispatcher via the back-pointer.
  std::unique_ptr<BPMNOS::Execution::Recorder> logger;   ///< Owned sink (built and subscribed by the caller, moved in via the constructor) for baseline entries; null = print plain progress lines. Used by the rollout dispatchers via the back-pointer.
  ThreadPool threadPool;         ///< Shared pool the rollout dispatchers run their rollouts on (sized config.threads).
  DecisionCounter decisionCounter;   ///< counts rolled-out decisions on the main engine; consulted by cutoff() (subscribed only when config.cutoff != 0)
  std::vector< std::unique_ptr<BPMNOS::Execution::EventDispatcher> > dispatchers;   ///< Dispatched first-feasible in priority order.
};

} // namespace BPMNOS::Rollout
