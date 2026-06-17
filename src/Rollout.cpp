#include "Rollout.h"

using namespace BPMNOS::Execution;

namespace BPMNOS::Rollout {

Rollout::Rollout( const std::shared_ptr<BPMNOS::Execution::Decision>& decision, const BPMNOS::Execution::SystemState* systemState, BPMNOS::Execution::Evaluator* evaluator )
  : evaluator(evaluator)
{
}

const SystemState* Rollout::getSystemState() const {
  // Placeholder: no forward simulation yet; The sub-engine simulation
  // (build engine + GreedyController + handlers, resume to termination, expose the final state) lands in
  // Step 2, owned here so the heavy code never enters a header.
  return nullptr;
}

} // namespace BPMNOS::Rollout
