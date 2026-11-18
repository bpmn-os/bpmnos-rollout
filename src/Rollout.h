#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace BPMNOS::Rollout {

/**
 * @brief Performs a one-step lookahead rollout of a single selected decision or of the deferral of its decision point.
 *
 * Assumes a selected decision from the current state, or the deferral of the decision point of the selected decision,
 * and simulates forward with the greedy policy to termination, the decision points deferred by the controller rolling
 * out being deferred in the simulation as well, each ending at its first reconsideration in the simulation,
 * which bisects a choice allowing at least the given number of values, as the controller rolling out does,
 * exposing the resulting final system state. The rollout runs in a private sub-engine on its own fork of
 * the scenario, into which the current system state is copied. Independent of the candidate source and the results type, so
 * it is a plain (non-template) class compiled once. The returned state is valid only while this Rollout is
 * alive: the caller reads it and folds it into its results before the Rollout is destroyed, which frees
 * the underlying sub-engine.
 */
class Rollout {
public:
  /// The engine is not internally synchronized; copyMutex (owned by the dispatcher and shared across the
  /// dispatch's parallel rollouts) serializes the deep copy of the shared source state, which reads the
  /// engine's lazily-pruning containers that mutate on read.
  /// The decision points deferred are each identified by the token of one of their pending requests in the foreign state,
  /// and with `deferral` the decision point of the selected decision is deferred instead of the decision being taken.
  Rollout( const std::shared_ptr<BPMNOS::Execution::Decision>& decision, bool deferral, const std::vector<const BPMNOS::Execution::Token*>& deferredTokens, const BPMNOS::Execution::SystemState* systemState, std::shared_ptr<BPMNOS::Execution::Evaluator> evaluator, std::optional<size_t> bisect, unsigned int index, std::mutex& copyMutex );

  /// Final state reached by the rollout (the copied system state, valid while this Rollout lives).
  const BPMNOS::Execution::SystemState* getSystemState() const;

private:
  BPMNOS::Execution::Deferrals deferrals;                       ///< the decision points deferred in the sub-engine; declared first, so that it outlives the engine and the greedy policy it is subscribed to
  BPMNOS::Execution::Engine engine;                             ///< sub-engine the rollout runs in
  BPMNOS::Execution::GreedyController greedyController;         ///< greedy base policy simulated in the sub-engine
  std::shared_ptr<BPMNOS::Execution::Evaluator> evaluator;  ///< shared with the greedy policy simulated here, and with whoever else evaluates through it
  std::shared_ptr<BPMNOS::Execution::Decision> decision;       ///< the selected decision translated onto the copied state
  std::shared_ptr<BPMNOS::Execution::Decision> cloneDecision( const std::shared_ptr<BPMNOS::Execution::Decision>& original ); ///< translate the selected decision onto the engine's copied state
  const BPMNOS::Execution::Token* findToken( const BPMNOS::Execution::Token* original ); ///< the token of the copied state awaiting the same kind of decision at the same instance and node as the given token of the foreign state
};

} // namespace BPMNOS::Rollout
