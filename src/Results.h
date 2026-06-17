#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <vector>
#include <compare>

namespace BPMNOS::Rollout {

/**
 * @brief Outcomes of repeated rollouts of a single decision candidate.
 *
 * Each recorded rollout contributes one weighted objective; the expected value is their mean.
 * Results are ordered by expected value (higher is better), so the candidate with the best expected
 * value compares greatest and can be selected with `std::max_element`. A Results with no recorded
 * rollout has the lowest possible expected value, so it never compares best.
 */
class Results {
public:
  std::vector<double> weightedObjectives; ///< Weighted objective of each recorded rollout.
  void add(const BPMNOS::Execution::SystemState* systemState);

  /// Order by expected value: a better-expected result compares greater; equality means equal expected value.
  std::partial_ordering operator<=>(const Results& other) const;
  bool operator==(const Results& other) const;

private:
  double totalWeightedObjective = 0.0;
};

} // namespace BPMNOS::Rollout
