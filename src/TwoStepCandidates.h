#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <tuple>
#include <memory>
#include <optional>
#include <type_traits>

namespace BPMNOS::Rollout {

/**
 * @brief Reward-ordered candidate decisions of the next contested decision at the bound system state.
 *
 * A contested decision is one the greedy base policy does not dispatch unambiguously: a choice, or a
 * competing sequential-ad-hoc-subprocess entry or message delivery. This collection merges the engine's
 * `FirstEnumeratedChoice` and `CompetingCandidates` into a single reward-ordered, lazily evaluated view,
 * with choice taking priority: if the highest-priority pending choice has a feasible alternative the view is
 * that choice's alternatives, otherwise it is the competing entries and message deliveries. Iterating
 * triggers evaluation against the currently bound system state and yields, best first (infeasible last),
 * `(reward, weak Event, weak Evaluation)` — all a consumer needs to force or dispatch the decision. It
 * parallels `CompetingCandidates`, one priority level up.
 */
class TwoStepCandidates {
public:
  TwoStepCandidates(BPMNOS::Execution::Evaluator* evaluator)
    : choice(evaluator)
    , competing(evaluator)
  {
  }

  /// Connect both owned collections so each registers itself for the observables it needs.
  void connect(BPMNOS::Execution::Notifier* notifier) {
    choice.connect(notifier);
    competing.connect(notifier);
  }

  /// Element yielded: reward, the decision's weak Event, its weak Evaluation.
  using value_type = std::tuple< double, std::weak_ptr<BPMNOS::Execution::Event>, std::weak_ptr<BPMNOS::Execution::Evaluation> >;

  /// End marker.
  struct Sentinel {};

  /// Iterates the selected underlying range (choice alternatives or the competing merge), projecting each
  /// element onto the common value_type.
  class Iterator {
  public:
    using ChoiceIterator = decltype( std::declval<BPMNOS::Execution::FirstEnumeratedChoice&>().begin() );
    using CompetingIterator = decltype( std::declval<BPMNOS::Execution::CompetingCandidates&>().begin() );

    /// Iterate the choice alternatives.
    Iterator(ChoiceIterator choiceBegin, ChoiceIterator choiceEnd)
      : useChoice(true), choiceIt(choiceBegin), choiceEnd(choiceEnd)
    {
    }
    /// Iterate the competing entries and message deliveries.
    explicit Iterator(CompetingIterator competingBegin)
      : useChoice(false), competingIt(competingBegin)
    {
    }

    bool operator!=(Sentinel) const {
      return useChoice ? ( choiceIt.value() != choiceEnd.value() )
                       : ( competingIt.value() != BPMNOS::Execution::CompetingCandidates::Sentinel{} );
    }

    value_type operator*() const {
      if ( useChoice ) {
        const auto& candidate = *choiceIt.value();
        constexpr std::size_t size = std::tuple_size_v< std::remove_cvref_t<decltype(candidate)> >;
        return value_type{ std::get<0>(candidate), std::get<size - 2>(candidate), std::get<size - 1>(candidate) };
      }
      return *competingIt.value();
    }

    Iterator& operator++() {
      if ( useChoice ) ++choiceIt.value(); else ++competingIt.value();
      return *this;
    }

  private:
    bool useChoice;
    std::optional<ChoiceIterator> choiceIt, choiceEnd;
    std::optional<CompetingIterator> competingIt;
  };

  /// Evaluate the choice source first; if it has a feasible front, iterate its alternatives, otherwise
  /// iterate the competing entries and message deliveries.
  Iterator begin();
  Sentinel end() { return {}; }

private:
  BPMNOS::Execution::FirstEnumeratedChoice choice;
  BPMNOS::Execution::CompetingCandidates competing;
};

} // namespace BPMNOS::Rollout
