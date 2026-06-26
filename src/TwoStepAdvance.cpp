#include "TwoStepAdvance.h"
#include <limits>
#include <stdexcept>
#include <cassert>

using namespace BPMNOS::Execution;
using namespace BPMNOS::Model;

namespace BPMNOS::Rollout {

GreedyPrefixController::GreedyPrefixController( Evaluator* evaluator )
  : candidates(evaluator)
{
  // Greedy prefix only: exit, then non-sequential entry, then instant direct message delivery. These never
  // branch, so the sub-engine proceeds exactly as the greedy base policy would; the contested choice and the
  // competing sequential-ad-hoc entries and message deliveries are deliberately not dispatched, so the engine
  // halts (below) at the next contested decision.
  dispatchers.push_back( std::make_unique<GreedyDispatcher<FirstFeasibleExit>>(evaluator) );
  dispatchers.push_back( std::make_unique<GreedyDispatcher<FirstFeasibleEntry>>(evaluator) );
  dispatchers.push_back( std::make_unique<InstantDirectMessage>() );
}

void GreedyPrefixController::connect( Mediator* mediator ) {
  for ( auto& eventDispatcher : dispatchers ) {
    eventDispatcher->connect(this);
  }
  candidates.connect(this);   // the controller relays Type::SystemState and the request notices to the candidates
  Controller::connect(mediator);
}

std::shared_ptr<Event> GreedyPrefixController::dispatchEvent( const SystemState* systemState ) {
  // Dispatch the first feasible greedy-prefix decision; forward any non-decision event immediately.
  for ( auto& eventDispatcher : dispatchers ) {
    if ( auto event = eventDispatcher->dispatchEvent(systemState) ) {
      if ( auto greedyDecision = std::dynamic_pointer_cast<Decision>(event) ) {
        if ( greedyDecision->reward().has_value() ) {
          return event;
        }
      }
      else {
        return event;
      }
    }
  }

  // No greedy-prefix decision is possible now. If a feasible contested decision is pending, halt the engine at
  // this branch point so the caller can roll out the second step. Otherwise — e.g. while waiting for a timer,
  // or for a contested decision that is only infeasible for now — dispatch nothing and let the engine increase
  // time; eventually a decision becomes required (dispatched or halted here) or all processes complete.
  auto candidate = candidates.begin();
  if ( candidate != candidates.end()
    && std::get<0>(*candidate) > -std::numeric_limits<double>::infinity() )
  {
    return std::make_shared<TerminationEvent>();
  }
  return nullptr;
}

TwoStepAdvance::TwoStepAdvance( const std::shared_ptr<Decision>& selectedDecision, const SystemState* foreignState, Evaluator* evaluator, unsigned int index, std::mutex& copyMutex )
  : scenario(forkScenario(foreignState, index))
  , controller(evaluator)
  , evaluator(evaluator)
{
  // Connect the sub-engine's greedy prefix before installing the state, so its cached candidate sources are
  // subscribed when initializeSystemState announces the state and can rebuild from its pending decisions.
  controller.connect(&engine);
  timeHandler.connect(&engine);

  // Install a copy of the current state under copyMutex: the deep copy reads the shared foreign state's
  // lazily-pruning containers, which erase expired entries on read, so concurrent advances of one dispatch
  // must not copy at the same time. The forked scenario above is a read-only copy, hence outside the lock;
  // cloneDecision and the simulation below run on this advance's private copy, also outside the lock.
  {
    std::lock_guard<std::mutex> lock(copyMutex);
    engine.initializeSystemState(scenario, foreignState);
  }
  decision = cloneDecision(selectedDecision);
  // Force the decision, then simulate forward; the controller halts at the next contested decision (or the run
  // completes if none remains).
  engine.resume(decision);
}

const BPMNOS::Model::Scenario* TwoStepAdvance::forkScenario(const BPMNOS::Execution::SystemState* systemState, unsigned int index) {
  if ( auto* stochasticScenario = dynamic_cast<const StochasticScenario*>(systemState->scenario) ) {
    // Fork at spawnTime so future values resample independently; offset the seed by the repetition index
    // so the same index yields the same resampled future across candidates (common random numbers). The
    // +1 keeps every advance off the base seed (index 0), which is the future the live run will realize.
    unsigned int seed = stochasticScenario->getSeed() + index + 1;
    forkedScenario = std::make_unique<StochasticScenario>( const_cast<StochasticScenario*>(stochasticScenario), systemState->getTime() + 1, seed );
    return forkedScenario.get();
  }
  // Deterministic scenarios are cloned so each sub-engine gets its own per-run memoization maps
  // (taskCompletionStatus / activityArrivalStatus); sharing one scenario would race under parallel advances.
  forkedScenario = systemState->scenario->clone();
  return forkedScenario.get();
}

std::shared_ptr<BPMNOS::Execution::Decision> TwoStepAdvance::cloneDecision( const std::shared_ptr<BPMNOS::Execution::Decision>& original ) {
  // The token is unambiguously identified by its instance identifier and node, both stable across the copy.
  const Token* originalToken = original->token;
  auto instanceId = originalToken->getInstanceId();
  const BPMN::FlowNode* node = originalToken->node;
  auto* systemState = engine.getSystemState();   // the copy installed by initializeSystemState

  // Find the equivalent token in a pending-decision list of the copied state. The (instance, node) pair must
  // identify it unambiguously, and — because the copy is a deep copy of the foreign state — the corresponding
  // token must carry an identical status; otherwise the advance would force a decision on a different state.
  auto findToken = [instanceId, node, originalToken]( auto& pending ) -> const BPMNOS::Execution::Token* {
    const BPMNOS::Execution::Token* found = nullptr;
    for ( auto& [token_ptr,_] : pending ) {
      if ( auto token = token_ptr.lock() ) {
        if ( token->getInstanceId() == instanceId && token->node == node ) {
          assert( !found && "TwoStepAdvance: ambiguous token match — more than one pending token has this instance and node" );
          found = token.get();
        }
      }
    }
    if ( !found ) {
      throw std::logic_error("TwoStepAdvance: cannot find token at node '" + node->id + "' for instance '" + BPMNOS::to_string(instanceId, STRING) + "'");
    }
    assert( found->node == originalToken->node && "TwoStepAdvance: cloned token has a different node than the original" );
    assert( found->status == originalToken->status && "TwoStepAdvance: cloned token status differs from the original (deep copy not faithful)" );
    return found;
  };

  // Create the equivalent decision for the copied token, carrying the same data.
  if ( dynamic_cast<BPMNOS::Execution::EntryDecision*>(original.get()) ) {
    if ( auto* token = findToken(systemState->pendingEntryDecisions) ) {
      return std::make_shared<EntryDecision>(token, evaluator);
    }
  }
  else if ( dynamic_cast<BPMNOS::Execution::ExitDecision*>(original.get()) ) {
    if ( auto* token = findToken(systemState->pendingExitDecisions) ) {
      return std::make_shared<ExitDecision>(token, evaluator);
    }
  }
  else if ( auto* choice = dynamic_cast<BPMNOS::Execution::ChoiceDecision*>(original.get()) ) {
    if ( auto* token = findToken(systemState->pendingChoiceDecisions) ) {
      return std::make_shared<ChoiceDecision>(token, choice->choices, evaluator);
    }
  }
  else if ( auto* messageDelivery = dynamic_cast<BPMNOS::Execution::MessageDeliveryDecision*>(original.get()) ) {
    auto* token = findToken(systemState->pendingMessageDeliveryDecisions);
    auto originalMessage = messageDelivery->message.lock();
    if ( token && originalMessage ) {
      // The message is identified by its origin (a node pointer stable across the copy) and the sender
      // identifier carried in its header; only a created (not yet delivered or withdrawn) message qualifies.
      auto origin = originalMessage->origin;
      const auto& sender = originalMessage->header[BPMNOS::Model::MessageDefinition::Index::Sender];
      assert(sender.has_value());
      // The (origin, sender) pair must identify a single created message in the copy; ambiguity would let the
      // advance force delivery of the wrong message.
      const Message* match = nullptr;
      for ( auto& message : systemState->messages ) {
        if ( message->state == Message::State::CREATED && message->origin == origin
          && message->header[BPMNOS::Model::MessageDefinition::Index::Sender] == sender )
        {
          assert( !match && "TwoStepAdvance: ambiguous message match — more than one created message has this origin and sender" );
          match = message.get();
        }
      }
      if ( !match ) {
        throw std::logic_error("TwoStepAdvance: cannot find message with origin '" + origin->id + "' sent from '" + BPMNOS::to_string(sender.value(), STRING) + "'");
      }
      return std::make_shared<MessageDeliveryDecision>(token, match, evaluator);
    }
  }
  throw std::logic_error("TwoStepAdvance: unexpected error");
}

const SystemState* TwoStepAdvance::getSystemState() const {
  return engine.getSystemState();
}

TwoStepCandidates& TwoStepAdvance::contestedDecisions() {
  return controller.contestedDecisions();
}

} // namespace BPMNOS::Rollout
