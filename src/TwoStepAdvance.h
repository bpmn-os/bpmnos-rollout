#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include "TwoStepCandidates.h"
#include <memory>
#include <vector>
#include <mutex>

namespace BPMNOS::Rollout {

/**
 * @brief Greedy-prefix controller that halts a sub-engine at the next contested decision.
 *
 * Dispatches the greedy prefix — first feasible exit, first feasible non-sequential entry, instant direct
 * message delivery — so the sub-engine proceeds exactly as the greedy base policy would. Once the prefix
 * yields nothing and a feasible contested decision (choice, or a competing sequential-ad-hoc entry or message
 * delivery) is pending, it emits a TerminationEvent to halt the engine at that branch point. When neither a
 * prefix decision nor a feasible contested decision is available it yields nullptr, letting the clock advance
 * — just as the greedy policy waits for running tasks to complete. The contested candidates at the halted
 * state are exposed for enumerating the second-step alternatives.
 */
class GreedyPrefixController : public BPMNOS::Execution::Controller {
public:
  GreedyPrefixController(BPMNOS::Execution::Evaluator* evaluator);
  void connect(BPMNOS::Execution::Mediator* mediator) override;
  TwoStepCandidates& contestedDecisions() { return candidates; }

protected:
  std::shared_ptr<BPMNOS::Execution::Event> dispatchEvent(const BPMNOS::Execution::SystemState* systemState) override;

private:
  std::vector< std::unique_ptr<BPMNOS::Execution::EventDispatcher> > dispatchers; ///< greedy prefix, dispatched first-feasible in priority order
  TwoStepCandidates candidates;   ///< the next contested decision's candidates: consulted to detect the branch point and enumerated for the second step
};

/**
 * @brief Forces a selected first-step decision and simulates forward to the next contested decision.
 *
 * Assumes a selected decision from the current state, copies that state into a private sub-engine on a forked
 * scenario, forces the decision, and simulates forward under the greedy prefix policy until the next contested
 * decision is reached (or until termination if none remains). It exposes that state and its contested
 * candidate decisions, so a caller can branch over the second step. Independent of the candidate source and
 * the results type, so it is a plain (non-template) class compiled once. The exposed state is valid only while
 * this TwoStepAdvance is alive: the caller reads it (and rolls out its candidates) before the TwoStepAdvance
 * is destroyed, which frees the underlying sub-engine.
 */
class TwoStepAdvance {
public:
  /// The engine is not internally synchronized; copyMutex (owned by the dispatcher and shared across the
  /// dispatch's parallel advances) serializes the deep copy of the shared source state, which reads the
  /// engine's lazily-pruning containers that mutate on read.
  TwoStepAdvance( const std::shared_ptr<BPMNOS::Execution::Decision>& decision, const BPMNOS::Execution::SystemState* systemState, BPMNOS::Execution::Evaluator* evaluator, unsigned int index, std::mutex& copyMutex );

  /// State reached at the next contested decision (or termination); valid while this TwoStepAdvance lives.
  const BPMNOS::Execution::SystemState* getSystemState() const;

  /// The second-step candidate decisions at that state (empty range if it is terminal / no feasible contested
  /// decision is pending).
  TwoStepCandidates& contestedDecisions();

private:
  std::unique_ptr<BPMNOS::Model::Scenario> forkedScenario;       ///< owns the per-advance scenario (stochastic fork or deterministic clone); declared first so it outlives the engine
  const BPMNOS::Model::Scenario* scenario;                       ///< scenario the advance runs on: the fork if forked, else the source
  BPMNOS::Execution::Engine engine;                             ///< sub-engine the advance runs in
  GreedyPrefixController controller;                            ///< greedy prefix policy halting at the next contested decision
  BPMNOS::Execution::TimeWarp timeHandler;                     ///< clock handler for the sub-engine
  BPMNOS::Execution::Evaluator* evaluator;
  std::shared_ptr<BPMNOS::Execution::Decision> decision;       ///< the selected decision translated onto the copied state
  const BPMNOS::Model::Scenario* forkScenario(const BPMNOS::Execution::SystemState* systemState, unsigned int index); ///< fork a stochastic scenario (seed offset by index) or clone a deterministic one (owned by forkedScenario) and return its pointer
  std::shared_ptr<BPMNOS::Execution::Decision> cloneDecision( const std::shared_ptr<BPMNOS::Execution::Decision>& original ); ///< translate the selected decision onto the engine's copied state
};

} // namespace BPMNOS::Rollout
