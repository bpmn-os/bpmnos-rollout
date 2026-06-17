#include "Results.h"

using namespace BPMNOS::Rollout;

void Results::add(const BPMNOS::Execution::SystemState* systemState) {
  weightedObjectives.push_back((double)systemState->getWeightedObjective());
  totalWeightedObjective += weightedObjectives.back();
}

std::partial_ordering Results::operator<=>(const Results& other) const {
  // An empty result (no rollout recorded) compares lowest, so it is never selected as best;
  // two empty results are equivalent.
  bool noResults = weightedObjectives.empty();
  bool noOtherResults = other.weightedObjectives.empty();
  if ( noResults || noOtherResults ) {
    return ( noResults == noOtherResults ) ? std::partial_ordering::equivalent
         : noResults                       ? std::partial_ordering::less
                                           : std::partial_ordering::greater;
  }

  return totalWeightedObjective / (double)weightedObjectives.size() <=> other.totalWeightedObjective / (double)other.weightedObjectives.size();
}

bool Results::operator==(const Results& other) const {
  // Two empty results are equal; an empty and a non-empty are not.
  if ( weightedObjectives.empty() || other.weightedObjectives.empty() ) {
    return weightedObjectives.empty() == other.weightedObjectives.empty();
  }

  return totalWeightedObjective / (double)weightedObjectives.size() == other.totalWeightedObjective / (double)other.weightedObjectives.size();
}

