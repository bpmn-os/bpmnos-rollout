#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <memory>
#include <mutex>

namespace BPMNOS::Rollout {

/**
 * @brief Performs a one-step lookahead rollout of a single selected decision.
 *
 * Assumes a selected decision from the current state and simulates forward with the greedy policy to termination,
 * exposing the resulting final system state. The rollout runs in a private sub-engine on a forked scenario,
 * into which the current system state is copied. Independent of the candidate source and the results type, so
 * it is a plain (non-template) class compiled once. The returned state is valid only while this Rollout is
 * alive: the caller reads it and folds it into its results before the Rollout is destroyed, which frees
 * the underlying sub-engine.
 */
class Rollout {
public:
  /// The engine is not internally synchronized; copyMutex (owned by the dispatcher and shared across the
  /// dispatch's parallel rollouts) serializes the deep copy of the shared source state, which reads the
  /// engine's lazily-pruning containers that mutate on read.
  Rollout( const std::shared_ptr<BPMNOS::Execution::Decision>& decision, const BPMNOS::Execution::SystemState* systemState, BPMNOS::Execution::Evaluator* evaluator, unsigned int index, std::mutex& copyMutex );

  /// Final state reached by the rollout (the copied system state, valid while this Rollout lives).
  const BPMNOS::Execution::SystemState* getSystemState() const;

private:
  std::unique_ptr<BPMNOS::Model::Scenario> forkedScenario;       ///< owns the per-rollout scenario (stochastic fork or deterministic clone); declared first so it outlives the engine
  const BPMNOS::Model::Scenario* scenario;                       ///< scenario the rollout runs on: the fork if forked, else the source
  BPMNOS::Execution::Engine engine;                             ///< sub-engine the rollout runs in
  BPMNOS::Execution::GreedyController greedyController;         ///< greedy base policy simulated in the sub-engine
  BPMNOS::Execution::TimeWarp timeHandler;                     ///< clock handler for the sub-engine
  BPMNOS::Execution::Evaluator* evaluator;
  std::shared_ptr<BPMNOS::Execution::Decision> decision;       ///< the selected decision translated onto the copied state
  const BPMNOS::Model::Scenario* forkScenario(const BPMNOS::Execution::SystemState* systemState, unsigned int index); ///< fork a stochastic scenario (seed offset by index) or clone a deterministic one (owned by forkedScenario) and return its pointer
  std::shared_ptr<BPMNOS::Execution::Decision> cloneDecision( const std::shared_ptr<BPMNOS::Execution::Decision>& original ); ///< translate the selected decision onto the engine's copied state
};

} // namespace BPMNOS::Rollout
