#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <cstddef>

namespace BPMNOS::Rollout {

/**
 * @brief Observer counting the decisions the rollout branches on (rolls out), dispatched on an engine.
 *
 * Only the decisions the RolloutController rolls out are counted; exits, ordinary entries, and explicitly
 * addressed (direct) messages are dispatched greedily and ignored. The classification mirrors the
 * controller's routing so the count equals the number of decisions actually branched — identically on the
 * main engine and on a rollout sub-engine:
 *  - every choice is rolled out;
 *  - an exit is never rolled out (always FirstFeasibleExit);
 *  - an entry is rolled out only if it enters a child of a SequentialAdHocSubProcess (the
 *    FirstFeasibleEntry / SequentialEntries split);
 *  - a message delivery is rolled out unless it is "direct" (explicitly addressed) — the exact test used by
 *    InstantDirectMessage, evaluated on the recipient token.
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
  /// True iff this decision is counted, i.e. one the rollout branches on, mirroring the controller's routing.
  static bool isCounted(const BPMNOS::Execution::Decision* decision);

  std::size_t decisions = 0;
};

} // namespace BPMNOS::Rollout
