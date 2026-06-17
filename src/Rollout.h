#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <memory>

namespace BPMNOS::Rollout {

/**
 * @brief Performs a one-step lookahead rollout of a single selected decision.
 *
 * Assumes a selected decision from the current state and simulates forward with the greedy policy to termination,
 * exposing the resulting final system state. Independent of the candidate source and the results type, so
 * it is a plain (non-template) class compiled once. The returned state is valid only while this Rollout is
 * alive: the caller reads it and folds it into its results before the Rollout is destroyed, which frees
 * the underlying sub-engine.
 *
 * Placeholder for now: the constructor performs no simulation and getSystemState() returns nullptr, so the
 * dispatcher skips results aggregation until the real sub-engine simulation lands.
 */
class Rollout {
public:
  Rollout( const std::shared_ptr<BPMNOS::Execution::Decision>& decision, const BPMNOS::Execution::SystemState* systemState, BPMNOS::Execution::Evaluator* evaluator );

  /// Final state reached by the rollout (valid while this Rollout lives); nullptr in the placeholder.
  const BPMNOS::Execution::SystemState* getSystemState() const;

private:
  BPMNOS::Execution::Evaluator* evaluator;
};

} // namespace BPMNOS::Rollout
