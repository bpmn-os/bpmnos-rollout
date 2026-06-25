#include "Results.h"
#include <cmath>
#include <stdexcept>
#include <format>
#include <algorithm>

using namespace BPMNOS::Rollout;

// One-sided Student-t critical values
/*
The values below are the output of this Julia script:

```julia
using Distributions
for alpha in (0.01, 0.05)
    t = [quantile(TDist(nu), 1 - alpha) for nu in 1:20]   # one-sided t_{1-alpha, nu}
    z =  quantile(Normal(), 1 - alpha)                    # normalLimit = z_{1-alpha}
    println("alpha = $alpha")
    println("  t = ", round.(t, digits=3))
    println("  normalLimit = ", round(z, digits=3))
end
```
*/
Results::SignificanceLevel Results::level_1 = {
  0.01,
  { 31.821, 6.965, 4.541, 3.747, 3.365, 3.143, 2.998, 2.896, 2.821, 2.764,
     2.718, 2.681, 2.650, 2.624, 2.602, 2.583, 2.567, 2.552, 2.539, 2.528 },
  2.326
};
Results::SignificanceLevel Results::level_n = {
  0.05,
  { 6.314, 2.920, 2.353, 2.132, 2.015, 1.943, 1.895, 1.860, 1.833, 1.812,
    1.796, 1.782, 1.771, 1.761, 1.753, 1.746, 1.740, 1.734, 1.729, 1.725 },
  1.645
};

double Results::SignificanceLevel::critical(std::size_t degreesOfFreedom) const {
  if ( degreesOfFreedom < 1 ) throw std::logic_error("SignificanceLevel::critical: degrees of freedom must be at least one");
  if ( degreesOfFreedom <= t.size() ) return t[degreesOfFreedom - 1];
  return normalLimit;
}

void Results::add(const BPMNOS::Execution::SystemState* systemState) {
  // Lightweight: only what selection always needs. Early-stopping statistics are derived lazily in
  // dominates, so repetitions == 1 (never early-stopped) incurs no extra per-rollout cost here.
  weightedObjectives.push_back((double)systemState->getWeightedObjective());
  totalWeightedObjective += weightedObjectives.back();
}

std::string Results::stringify() const {
  if ( weightedObjectives.empty() ) {
    return std::string("n/a");
  }
  
  std::string summary = std::format("{}", mean());
  // Show the spread only when there is more than one rollout to span (a single rollout's min and max
  // are just its mean, so the bracket would add nothing).
  if ( weightedObjectives.size() > 1 ) {
    auto [min, max] = std::ranges::minmax(weightedObjectives);
    summary += std::format("\t[{},{}]", min, max);
  }
  return summary;
}

nlohmann::ordered_json Results::jsonify() const {
  if ( weightedObjectives.size() == 1 ) {
    return { "objective", weightedObjectives.front() };
  }
  if ( weightedObjectives.size() > 1 ) {
    return {
      { "mean", mean() },
      { "objectives", weightedObjectives }
    };
  }
  return {};
}

double Results::mean() const {
  return totalWeightedObjective / (double)weightedObjectives.size();
}

double Results::standardDeviation() const {
  std::size_t n = weightedObjectives.size();
  if ( n < 2 ) throw std::logic_error("Results::standardDeviation requires at least two rollouts");
  double m = mean();
  double sumSquaredDeviations = 0.0;
  for ( double objective : weightedObjectives ) {
    double deviation = objective - m;
    sumSquaredDeviations += deviation * deviation;
  }
  return std::sqrt(sumSquaredDeviations / (double)(n - 1));
}

double Results::lowerPredictionBound() const {
  std::size_t k = weightedObjectives.size();
  // The incumbent must be fully sampled; with fewer than two rollouts the bound is undefined,
  // and silently disabling it would let catastrophic candidates pass unnoticed.
  if ( k < 2 ) throw std::logic_error("Results::lowerPredictionBound: incumbent needs at least two rollouts");
  return mean() - level_1.critical(k - 1) * standardDeviation() * std::sqrt(1.0 + 1.0 / (double)k);
}

double Results::meanUpperBound() const {
  std::size_t n = weightedObjectives.size();
  if ( n < 2 ) throw std::logic_error("Results::meanUpperBound requires at least two rollouts");
  return mean() + level_n.critical(n - 1) * standardDeviation() / std::sqrt((double)n);
}

bool Results::dominates(const Results& other) const {
  // Only invoked with repetitions > 1, right after the candidate records a rollout. The candidate must
  // have that run to assess; an empty candidate is a contract violation, not a state to silently skip.
  if ( other.weightedObjectives.empty() ) throw std::logic_error("Results::dominates: candidate has no rollout to assess");

  // Results are dominated if the performance of the last rollout is significnatly worse than the baseline.
  if ( other.weightedObjectives.back() < lowerPredictionBound() ) return true;

  // Other results are dominated if they are worse on averaged than the baseline.
  if ( other.weightedObjectives.size() >= 2 && other.meanUpperBound() < mean() ) return true;

  return false;
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
  return mean() <=> other.mean();
}

bool Results::operator==(const Results& other) const {
  // Two empty results are equal; an empty and a non-empty are not.
  if ( weightedObjectives.empty() || other.weightedObjectives.empty() ) {
    return weightedObjectives.empty() == other.weightedObjectives.empty();
  }
  return mean() == other.mean();
}
