#include "Rollout.h"
#include <stdexcept>
#include <cassert>

using namespace BPMNOS::Execution;
using namespace BPMNOS::Model;

namespace BPMNOS::Rollout {

Rollout::Rollout( const std::shared_ptr<Decision>& selectedDecision, const SystemState* foreignState, Evaluator* evaluator, unsigned int index, std::mutex& copyMutex )
  : scenario(forkScenario(foreignState, index))
  , greedyController(evaluator)
  , evaluator(evaluator)
{
  // Connect the sub-engine's greedy policy before installing the state, so its cached candidate sources are
  // subscribed when initializeSystemState announces the state and can rebuild from its pending decisions.
  greedyController.connect(&engine);
  timeHandler.connect(&engine);

  // Install a copy of the current state under copyMutex: the deep copy reads the shared foreign state's
  // lazily-pruning containers, which erase expired entries on read, so concurrent rollouts of one dispatch
  // must not copy at the same time. The forked scenario above is a read-only copy, hence outside the lock;
  // cloneDecision and the simulation below run on this rollout's private copy, also outside the lock.
  {
    std::lock_guard<std::mutex> lock(copyMutex);
    engine.initializeSystemState(scenario, foreignState);
  }
  decision = cloneDecision(selectedDecision);
  engine.resume(decision);
}

const BPMNOS::Model::Scenario* Rollout::forkScenario(const BPMNOS::Execution::SystemState* systemState, unsigned int index) {
  if ( auto* stochasticScenario = dynamic_cast<const StochasticScenario*>(systemState->scenario) ) {
    // Fork at spawnTime so future values resample independently; offset the seed by the repetition index
    // so the same index yields the same resampled future across candidates (common random numbers). The
    // +1 keeps every rollout off the base seed (index 0), which is the future the live run will realize —
    // rolling out on it would let the candidate peek at the actual outcome.
    unsigned int seed = stochasticScenario->getSeed() + index + 1;
    forkedScenario = std::make_unique<StochasticScenario>( const_cast<StochasticScenario*>(stochasticScenario), systemState->getTime() + 1, seed );
    return forkedScenario.get();
  }
  // Deterministic scenarios are cloned so each sub-engine gets its own per-run memoization maps
  // (taskCompletionStatus / activityArrivalStatus); sharing one scenario would race under parallel rollouts.
  forkedScenario = systemState->scenario->clone();
  return forkedScenario.get();
}


std::shared_ptr<BPMNOS::Execution::Decision> Rollout::cloneDecision( const std::shared_ptr<BPMNOS::Execution::Decision>& original ) {
  // The token is unambiguously identified by its instance identifier and node, both stable across the copy.
  const Token* originalToken = original->token;
  auto instanceId = originalToken->getInstanceId();
  const BPMN::FlowNode* node = originalToken->node;
  auto* systemState = engine.getSystemState();   // the copy installed by initializeSystemState

  // Find the equivalent token in a pending-decision list of the copied state. The (instance, node) pair must
  // identify it unambiguously, and — because the copy is a deep copy of the foreign state — the corresponding
  // token must carry an identical status; otherwise the rollout would force a decision on a different state.
  auto findToken = [instanceId, node, originalToken]( auto& pending ) -> const BPMNOS::Execution::Token* {
    const BPMNOS::Execution::Token* found = nullptr;
    for ( auto& [token_ptr,_] : pending ) {
      if ( auto token = token_ptr.lock() ) {
        if ( token->getInstanceId() == instanceId && token->node == node ) {
          assert( !found && "Rollout: ambiguous token match — more than one pending token has this instance and node" );
          found = token.get();
        }
      }
    }
    if ( !found ) {
      throw std::logic_error("Rollout: cannot find token at node '" + node->id + "' for instance '" + BPMNOS::to_string(instanceId, STRING) + "'");
    }
    assert( found->node == originalToken->node && "Rollout: cloned token has a different node than the original" );
    assert( found->status == originalToken->status && "Rollout: cloned token status differs from the original (deep copy not faithful)" );
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
      // rollout force delivery of the wrong message.
      const Message* match = nullptr;
      for ( auto& message : systemState->messages ) {
        if ( message->state == Message::State::CREATED && message->origin == origin
          && message->header[BPMNOS::Model::MessageDefinition::Index::Sender] == sender )
        {
          assert( !match && "Rollout: ambiguous message match — more than one created message has this origin and sender" );
          match = message.get();
        }
      }
      if ( !match ) {
        throw std::logic_error("Rollout: cannot find message with origin '" + origin->id + "' sent from '" + BPMNOS::to_string(sender.value(), STRING) + "'");
      }
      return std::make_shared<MessageDeliveryDecision>(token, match, evaluator);
    }
  }
  throw std::logic_error("Rollout: unexpected error");
}

const SystemState* Rollout::getSystemState() const {
  return engine.getSystemState();
}

} // namespace BPMNOS::Rollout
