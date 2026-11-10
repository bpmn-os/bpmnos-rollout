#include "DecisionCounter.h"

using namespace BPMNOS::Rollout;
using namespace BPMNOS::Execution;

void DecisionCounter::connect(Notifier* notifier) {
  notifier->addSubscriber(this, Observable::Type::Event);
}

const std::size_t& DecisionCounter::count() const {
  return decisions;
}

void DecisionCounter::notice(const Observable* observable) {
  if ( dynamic_cast<const Decision*>(observable) ) {
    ++decisions;
  }
}
