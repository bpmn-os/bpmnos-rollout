#pragma once

#include <bpmnos-execution.h>
#include <nlohmann/json.hpp>
#include <concepts>
#include <string>

namespace BPMNOS::Rollout {

/**
 * @brief Requirements on the rollout results policy type.
 *
 * A results type is default-constructible, records a rollout's final state via `add(const SystemState*)`,
 * and is comparable with `>` so the best candidate can be selected. It reports itself two ways for baseline
 * logging: `stringify()` yields a human-readable summary string, and `jsonify()` yields an
 * `nlohmann::ordered_json` entry. It is held only by `shared_ptr` and never copied, so it may carry
 * arbitrarily heavy statistics. An optional `dominates(const ResultsType&) const` lets a candidate's
 * remaining repetitions be cancelled early once the baseline provably beats it.
 */
template <typename T>
concept ResultsPolicy =
  std::default_initializable<T> &&
  requires(T results, const T& a, const T& b, const BPMNOS::Execution::SystemState* state) {
    results.add(state);
    { a > b } -> std::convertible_to<bool>;
    { a.stringify() } -> std::convertible_to<std::string>;
    { a.jsonify() } -> std::convertible_to<nlohmann::ordered_json>;
  };

// A rollout dispatcher holds a back-pointer to its owning controller and reads config / thread pool / baseline /
// the cut-off decision through it. The full definition is available at instantiation (RolloutController.h
// includes the dispatcher headers, defines the controller, and befriends the dispatchers).
template <ResultsPolicy ResultsType>
class RolloutController;

} // namespace BPMNOS::Rollout
