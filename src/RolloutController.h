#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include "Results.h"
#include "ThreadPool.h"
#include "RolloutDispatcher.h"
#include "DecisionCounter.h"
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <chrono>
#include <iostream>
#include <format>

namespace BPMNOS::Rollout {

/**
 * @brief Controller selecting decisions by one-step lookahead rollout.
 *
 * The engine itself enters every activity that is not a child of a sequential ad-hoc subprocess and
 * delivers every explicitly addressed message, so that the decisions left to the controller are the choices,
 * the entries of children of sequential ad-hoc subprocesses, and the deliveries of messages that are not
 * explicitly addressed. Each of them is made by the rollout mechanism, which iteratively applies candidate
 * decisions completing the forward simulation under a greedy policy, and dispatches the candidate yielding
 * the best final objective.
 *
 * The decisions compete without precedence: a single `RolloutDispatcher` takes the candidates of the choices, a choice
 * allowing at least `config.bisect` values being searched by bisection, of the entries and of the message deliveries,
 * sorts them by their rewards, and rolls out the first `config.candidates` options, which are these candidates and,
 * after the first candidate of every decision point for which the model states a deferral whose condition holds, the
 * deferral of that decision point, if a deferral is admissible. The controller holds the deferrals it places, subscribed
 * to the notifications it forwards, so that a deferral ends at its first reconsideration, and its collections withhold
 * the candidates of deferred decision points.
 *
 * Templated on the results type (the aggregation/selection policy); header-only so it never assumes a
 * particular results type. Every alternative is rolled out from the current state at every decision; the
 * greedy decision is valued by the baseline if a single repetition is made, assuming that a scenario whose
 * future is uncertain is always run with several, and is rolled out as every alternative otherwise. The baseline is held by `shared_ptr` and advanced (moved)
 * to the winner's results, so a heavy results type is never deep-copied.
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
    unsigned int threads = 1;     ///< number of threads used for rollouts (0 = all available hardware threads)
    std::optional<size_t> bisect = std::nullopt; ///< least number of values of a choice searched by bisection, by the rollout and by the greedy policy it simulates (none = only a decimal attribute without a step)
  };
  static Config default_config() { return {}; } // Workaround for compiler bug, as in GreedyController (a `Config config = {}` default argument fails to compile).

  RolloutController(std::shared_ptr<BPMNOS::Execution::Evaluator> evaluator, std::shared_ptr<ResultsType> greedyResults, Config config = default_config(), std::unique_ptr<BPMNOS::Execution::Recorder> logger = nullptr)
    : config(config)
    , baselineResults(std::move(greedyResults))
    , logger(std::move(logger))
    , threadPool(config.threads)
  {
    // the deferrals end at the reconsiderations the controller forwards to them
    deferrals.connect(this);
    // The choices, the sequential entries and the message deliveries compete in a single rollout dispatcher, which
    // advances the baseline in place to a winner's results.
    dispatchers.push_back( std::make_unique<RolloutDispatcher<ResultsType>>(evaluator, this) );

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
  // The rollout dispatchers hold a back-pointer to this controller, read config, threadPool, and cutoff()
  // through it, and advance baselineResults.
  template <ResultsPolicy R>
  friend class RolloutDispatcher;

  Config config;
  BPMNOS::Execution::Deferrals deferrals;   ///< The decision points deferred by the rollout, read by the collections of the dispatcher; declared before the dispatchers, which it outlives
  std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();   ///< construction time; anchors elapsedSeconds() (≈ start of the live run, as the controller is built just before engine.run).
  std::shared_ptr<ResultsType> baselineResults;   ///< Baseline reported as progress; starts as the greedy result from the start, reseated to the winner's result whenever a non-greedy candidate is dispatched. Values the greedy decision if a single repetition is made, which assumes that a scenario whose future is uncertain is always run with several, and no decision otherwise. Advanced by every rollout dispatcher via the back-pointer.
  std::unique_ptr<BPMNOS::Execution::Recorder> logger;   ///< Owned sink (built and subscribed by the caller, moved in via the constructor) for baseline entries; null = print plain progress lines. Used by the rollout dispatchers via the back-pointer.
  ThreadPool threadPool;         ///< Shared pool the rollout dispatchers run their rollouts on (sized config.threads).
  DecisionCounter decisionCounter;   ///< counts rolled-out decisions on the main engine; consulted by cutoff() (subscribed only when config.cutoff != 0)
  std::vector< std::unique_ptr<BPMNOS::Execution::EventDispatcher> > dispatchers;   ///< The rollout dispatcher.
};

} // namespace BPMNOS::Rollout
