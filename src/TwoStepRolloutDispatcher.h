#pragma once

#include <bpmn++.h>
#include <bpmnos-model.h>
#include <bpmnos-execution.h>
#include <nlohmann/json.hpp>
#include "ResultsPolicy.h"
#include "TwoStepCandidates.h"
#include "TwoStepAdvance.h"
#include "Rollout.h"
#include "ThreadPool.h"
#include <memory>
#include <limits>
#include <tuple>
#include <vector>
#include <list>
#include <mutex>
#include <future>
#include <cstddef>
#include <utility>
#include <iostream>
#include <format>

namespace BPMNOS::Rollout {

/**
 * @brief Dispatcher selecting a contested decision by two-step lookahead rollout.
 *
 * On each dispatch it collects the top feasible candidate decisions of the next contested decision in reward
 * order (the live first step). Every candidate — including the reward-order front (the greedy decision) — is
 * then valued by two-step lookahead: its decision is forced and the simulation advanced under the greedy
 * prefix to the next contested decision (a `TwoStepAdvance`), each of whose alternatives is rolled out to the
 * horizon under the greedy policy (a `Rollout`); the candidate's value is the best such second-step
 * continuation. Because the second step can change the assessment of the greedy front, the front is valued
 * like every other candidate rather than taken as a fixed baseline. The candidate with the best value is
 * dispatched (ties keep the greedy front). When a non-greedy candidate wins, the reported baseline is advanced
 * to that candidate's achievable greedy continuation (its front second-step rollout — the value of committing
 * the decision and then proceeding greedily).
 *
 * Templated on the results type only; the choice-versus-competing distinction is handled inside
 * `TwoStepCandidates`. Header-only so the results type stays an open policy parameter. Results are held by
 * `shared_ptr` and moved, never copied, so a heavy results type is never deep-copied.
 */
template <ResultsPolicy ResultsType>
class TwoStepRolloutDispatcher : public BPMNOS::Execution::EventDispatcher {
public:
  TwoStepRolloutDispatcher( BPMNOS::Execution::Evaluator* evaluator, RolloutController<ResultsType>* controller )
    : candidates(evaluator)
    , evaluator(evaluator)
    , controller(controller)
  {
    if ( controller->config.repetitions == 0 ) {
      throw std::invalid_argument("TwoStepRolloutDispatcher: repetitions must be at least 1");
    }
  }

  std::shared_ptr<BPMNOS::Execution::Event> dispatchEvent( const BPMNOS::Execution::SystemState* systemState ) override {
    using namespace BPMNOS::Execution;

    // Collect the top-`maxCandidates` feasible first-step (d1) candidate decisions in reward order.
    std::vector< std::shared_ptr<Decision> > decisions = collect( candidates, controller->config.candidates );
    if ( decisions.empty() ) {
      return nullptr;
    }

    // Once the controller's cut-off is reached, stop branching and dispatch the greedy front without rolling
    // out — exactly as a GreedyDispatcher would.
    if ( controller->cutoff() ) {
      return decisions.front();
    }

    // Value every candidate — including the greedy front — by two-step lookahead. `results` holds each
    // candidate's two-step value (best second step); `greedyContinuations` holds its achievable greedy
    // continuation (front second step), used to advance the reported baseline of the winner.
    std::vector< std::shared_ptr<ResultsType> > results( decisions.size() );
    std::vector< std::shared_ptr<ResultsType> > greedyContinuations( decisions.size() );
    for ( std::size_t i = 0; i < decisions.size(); ++i ) {
      results[i] = std::make_shared<ResultsType>();
      greedyContinuations[i] = std::make_shared<ResultsType>();
    }
    std::vector<std::mutex> resultMutexes( decisions.size() );
    while ( queues.size() < decisions.size() ) {
      queues.push_back( controller->threadPool.addQueue() );   // one queue per candidate, reused across dispatches
    }

    std::vector< std::future<void> > jobs;
    for ( std::size_t i = 0; i < decisions.size(); ++i ) {
      for ( unsigned int round = 0; round < controller->config.repetitions; ++round ) {
        jobs.push_back( controller->threadPool.submit( queues[i], [this, &decisions, &results, &greedyContinuations, &resultMutexes, systemState, i, round]() {
          // Advance from the shared live state through the first-step decision to the next contested decision.
          // The deep copy of the shared state is serialized by the dispatcher's copyMutex.
          TwoStepAdvance advance( decisions[i], systemState, evaluator, round, copyMutex );

          // Collect the second-step (d2) candidates at the reached state.
          std::vector< std::shared_ptr<Decision> > secondStep = collect( advance.contestedDecisions(), controller->config.candidates );

          if ( secondStep.empty() ) {
            // Terminal: no feasible contested decision remains, so the advance ran to completion. Its final
            // state is both the two-step value and the greedy continuation for this round.
            if ( auto* finalState = advance.getSystemState() ) {
              std::lock_guard lock( resultMutexes[i] );
              results[i]->add( finalState );
              greedyContinuations[i]->add( finalState );
            }
            return;
          }

          // Roll out each second-step alternative to the horizon under the greedy policy. The advance's state is
          // private to this job, so its leaf copies are serialized by a job-local mutex (uncontended). The best
          // leaf by objective is the round's two-step value; the reward-order front (the greedy second step) is
          // the round's achievable greedy continuation. Leaves are kept alive until both are folded in.
          std::mutex leafCopyMutex;
          std::list<Rollout> leaves;
          const SystemState* bestState = nullptr;
          const SystemState* greedyState = nullptr;
          double bestObjective = 0.0;
          for ( std::size_t j = 0; j < secondStep.size(); ++j ) {
            leaves.emplace_back( secondStep[j], advance.getSystemState(), evaluator, round, leafCopyMutex );
            auto* state = leaves.back().getSystemState();
            if ( !state ) {
              continue;
            }
            double objective = (double)state->getWeightedObjective();
            if ( j == 0 ) {
              greedyState = state;   // the reward-order front second step is the greedy continuation
            }
            if ( !bestState || objective > bestObjective ) {
              bestState = state;
              bestObjective = objective;
            }
          }

          std::lock_guard lock( resultMutexes[i] );
          if ( bestState ) {
            results[i]->add( bestState );
          }
          if ( greedyState ) {
            greedyContinuations[i]->add( greedyState );
          }
        }) );
      }
    }
    for ( auto& job : jobs ) {
      job.get();
    }
    // Clear every queue so the next dispatch starts from a clean pool.
    for ( auto& queue : queues ) {
      controller->threadPool.clearQueue( queue );
    }

    // Dispatch the candidate with the best two-step value; ties keep the greedy front (first maximum).
    std::size_t bestIndex = 0;
    for ( std::size_t i = 1; i < decisions.size(); ++i ) {
      if ( *results[i] > *results[bestIndex] ) {
        bestIndex = i;
      }
    }

    // A non-greedy candidate won: advance the reported baseline to its achievable greedy continuation — the
    // value of committing this decision and then proceeding greedily. Dispatching the greedy front leaves the
    // committed trajectory, and thus the baseline, unchanged. dispatchEvent runs serially on the main engine,
    // so reseating and reporting here is not racy.
    if ( bestIndex != 0 ) {
      controller->baselineResults = std::move( greedyContinuations[bestIndex] );
      if ( controller->logger ) {
        controller->logger->inject("rollout", nlohmann::ordered_json{ {"results", controller->baselineResults->jsonify()}, {"time", controller->elapsedSeconds()} });
      }
      else {
        std::cout << std::format("{:.1f}\t{}\n", controller->elapsedSeconds(), controller->baselineResults->stringify());
      }
    }
    return decisions[bestIndex];
  }

  void connect( BPMNOS::Execution::Mediator* mediator ) override {
    candidates.connect(mediator);   // the candidates register for the observables they need
    BPMNOS::Execution::EventDispatcher::connect(mediator);
  }

protected:
  /// Collect the top-`maxCandidates` feasible decisions (0 = all) of a contested-decision view, in reward order.
  static std::vector< std::shared_ptr<BPMNOS::Execution::Decision> > collect( TwoStepCandidates& source, unsigned int maxCandidates ) {
    using namespace BPMNOS::Execution;
    std::vector< std::shared_ptr<Decision> > decisions;
    for ( auto candidate : source ) {
      if ( std::get<0>(candidate) <= -std::numeric_limits<double>::infinity() ) {
        break;   // reward-ordered descending: infeasible candidates (-infinity) sort last
      }
      // value_type is (reward, weak Event, weak Evaluation): the decision is the weak Event at index 1.
      auto event = std::get<1>(candidate).lock();
      if ( !event ) {
        continue;   // decision expired before it could be assessed
      }
      auto decision = std::dynamic_pointer_cast<Decision>(event);
      if ( !decision ) {
        continue;
      }
      decisions.push_back( std::move(decision) );
      if ( maxCandidates && decisions.size() >= maxCandidates ) {
        break;
      }
    }
    return decisions;
  }

  TwoStepCandidates candidates;     ///< the live first-step contested-decision candidates
  BPMNOS::Execution::Evaluator* evaluator;  ///< passed to each TwoStepAdvance and Rollout
  RolloutController<ResultsType>* controller;   ///< owning controller; source of config, threadPool, baselineResults, logger, and the cut-off decision
  std::vector<ThreadPool::QueueId> queues;   ///< one queue per first-step candidate, grown lazily and reused across dispatches
  std::mutex copyMutex;             ///< serializes each advance's deep copy of the shared live state (the engine is not internally synchronized; the copy reads lazily-pruning containers that mutate on read)
};

} // namespace BPMNOS::Rollout
