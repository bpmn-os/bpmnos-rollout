#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <vector>
#include <array>
#include <compare>
#include <cstddef>
#include <string>

namespace BPMNOS::Rollout {

/**
 * @brief Outcomes of repeated rollouts of a single decision candidate.
 *
 * Records the weighted objective of each rollout; the expected value is their mean. Results are ordered
 * by expected value (higher is better), so the best candidate compares greatest and can be selected with
 * `std::max_element`. A Results with no recorded rollout has the lowest possible expected value, so it
 * never compares best.
 *
 * Acting as the incumbent (full repetitions), a Results also judges a candidate Results for early discard
 * via `dominates`, combining two prune-only tests — see there. Early-stopping statistics are computed only
 * inside `dominates`, so a run with `repetitions == 1` (which the dispatcher never early-stops) pays no
 * overhead beyond recording each objective.
 */
class Results {
public:
  std::vector<double> weightedObjectives; ///< Weighted objective of each recorded rollout.
  void add(const BPMNOS::Execution::SystemState* systemState);

  /// Human-readable summary: the mean, plus "[<min>,<max>]" when more than one rollout was recorded.
  std::string stringify() const;

  /// Order by expected value: a better-expected result compares greater; equality means equal expected value.
  std::partial_ordering operator<=>(const Results& other) const;
  bool operator==(const Results& other) const;

  /// True if this incumbent (full repetitions) should discard `other`, by either prune-only test:
  ///   1. the candidate's latest run is below the incumbent's one-sided lower t-prediction
  ///      bound for a single run, i.e. implausibly bad for the incumbent's own distribution. 
  ///   2. the candidate's upper t-confidence bound on its mean is below the
  ///      incumbent's mean, i.e. it cannot be expected to beat the incumbent on average. Needs n >= 2.
  bool dominates(const Results& other) const;

  /// A one-sided significance level bundled with its own Student-t critical values:
  /// `t[i]` is the critical value t_{1-alpha, dof = i + 1}, and
  /// `normalLimit` is the dof -> infinity limit z_{1-alpha} used for degrees of freedom beyond the table.
  struct SignificanceLevel {
    double alpha;                                            ///< one-sided significance level
    std::array<double, 20> t;                                ///< t_{1-alpha, dof} for dof = 1..20
    double normalLimit;                                      ///< z_{1-alpha}, used for dof > 20
    double critical(std::size_t degreesOfFreedom) const;     ///< t_{1-alpha, degreesOfFreedom}
  };

  /// Significance levels for the two early-stopping tests, shared by every Results.
  /// Read concurrently during rollouts: set them before running, not during.
  static SignificanceLevel level_1; ///< test 1: single-run prediction bound (catastrophe)
  static SignificanceLevel level_n; ///< test 2: candidate mean confidence bound (collective)

private:
  double mean() const;
  double standardDeviation() const;     ///< unbiased sample standard deviation; throws for fewer than two rollouts
  double lowerPredictionBound() const;  ///< incumbent role: mu - t(1-a,k-1)*s*sqrt(1+1/k); throws for k < 2
  double meanUpperBound() const;        ///< candidate role: mu + t(1-a,n-1)*s/sqrt(n); throws for n < 2

  double totalWeightedObjective = 0.0;
};

} // namespace BPMNOS::Rollout
