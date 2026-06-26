#include "TwoStepCandidates.h"
#include <limits>

namespace BPMNOS::Rollout {

TwoStepCandidates::Iterator TwoStepCandidates::begin() {
  // Choice has priority: if the highest-priority pending choice has a feasible alternative (the reward-ordered
  // front is not -infinity), branch over the choice alternatives. Otherwise branch over the competing
  // sequential-ad-hoc entries and message deliveries.
  auto choiceBegin = choice.begin();
  auto choiceEnd = choice.end();
  if ( choiceBegin != choiceEnd
    && std::get<0>(*choiceBegin) > -std::numeric_limits<double>::infinity() )
  {
    return Iterator(choiceBegin, choiceEnd);
  }
  return Iterator(competing.begin());
}

} // namespace BPMNOS::Rollout
