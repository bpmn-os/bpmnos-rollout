#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <cstddef>

namespace BPMNOS::Rollout {

/**
 * @brief Observer counting the decisions the rollout branches on (rolls out), dispatched on an engine.
 *
 * Every decision is counted. The engine itself enters the activities that are not children of a sequential
 * ad-hoc subprocess and delivers the explicitly addressed messages by events that are not decisions, so that
 * the decisions dispatched are the choices, the entries of children of sequential ad-hoc subprocesses and the
 * deliveries of messages that are not explicitly addressed, which are the decisions the RolloutController
 * rolls out. The count is thus the same on the main engine and on a rollout sub-engine.
 *
 * Subscribes to Observable::Type::Event (the Candidates sources' `connect(Notifier*)` pattern).
 */
class DecisionCounter : public BPMNOS::Execution::Observer {
public:
  /// Subscribe to the engine's dispatched events.
  void connect(BPMNOS::Execution::Notifier* notifier);

  /// Live count of rolled-out decisions, returned by reference so it can be shared with the dispatchers.
  const std::size_t& count() const;

  void notice(const BPMNOS::Execution::Observable* observable) override;

private:
  std::size_t decisions = 0;
};

} // namespace BPMNOS::Rollout
