#include "Rollout.h"
#include <stdexcept>
#include <cassert>

using namespace BPMNOS::Execution;
using namespace BPMNOS::Model;

namespace BPMNOS::Rollout {

Rollout::Rollout( const std::shared_ptr<Decision>& selectedDecision, const SystemState* foreignState, std::shared_ptr<Evaluator> evaluator, unsigned int index, std::mutex& copyMutex )
  : engine(foreignState->scenario->dataProvider->getModel())
  , greedyController(evaluator)
  , evaluator(std::move(evaluator))
{
  // Connect the sub-engine's greedy policy before installing the state, so its cached candidate sources are
  // subscribed when initializeSystemState announces the state and can rebuild from its pending decisions.
  greedyController.connect(&engine);

  // Every rollout runs on its own fork of the scenario, which agrees with the live run up to its current
  // time and is the index-th realization thereafter. The same index yields the same realization across
  // candidates (common random numbers), and no fork reproduces the future the live run will realize. A
  // data provider whose future is certain forks a run by a new scenario, which cannot differ from the run.
  // Forking only reads the scenario of the live run, hence outside the lock.
  auto scenario = foreignState->scenario->dataProvider->forkScenario(*foreignState->scenario, index);

  // Install a copy of the current state under copyMutex: the deep copy reads the shared foreign state's
  // lazily-pruning containers, which erase expired entries on read, so concurrent rollouts of one dispatch
  // must not copy at the same time. cloneDecision and the simulation below run on this rollout's private
  // copy, outside the lock.
  {
    std::lock_guard<std::mutex> lock(copyMutex);
    engine.initializeSystemState(std::move(scenario), foreignState);
  }
  decision = cloneDecision(selectedDecision);
  engine.resume(decision);
}

std::shared_ptr<BPMNOS::Execution::Decision> Rollout::cloneDecision( const std::shared_ptr<BPMNOS::Execution::Decision>& original ) {
  // The token is unambiguously identified by its instance identifier and node, both stable across the copy.
  auto originalToken = original->token.lock();
  assert( originalToken );
  auto instanceId = originalToken->getInstanceId();
  const BPMN::Node* node = originalToken->node;
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
    assert( found->decisionRequest && "Rollout: pending token has no decision request" );
    return found;
  };

  // Create the equivalent decision for the copied token, carrying the same data.
  if ( dynamic_cast<BPMNOS::Execution::EntryDecision*>(original.get()) ) {
    if ( auto* token = findToken(systemState->pendingEntryDecisions) ) {
      return std::make_shared<EntryDecision>(token->decisionRequest.get(), evaluator.get());
    }
  }
  else if ( dynamic_cast<BPMNOS::Execution::ExitDecision*>(original.get()) ) {
    if ( auto* token = findToken(systemState->pendingExitDecisions) ) {
      return std::make_shared<ExitDecision>(token->decisionRequest.get(), evaluator.get());
    }
  }
  else if ( auto* choice = dynamic_cast<BPMNOS::Execution::ChoiceDecision*>(original.get()) ) {
    if ( auto* token = findToken(systemState->pendingChoiceDecisions) ) {
      return std::make_shared<ChoiceDecision>(token->decisionRequest.get(), choice->choices, evaluator.get());
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
      return std::make_shared<MessageDeliveryDecision>(token->decisionRequest.get(), match, evaluator.get());
    }
  }
  throw std::logic_error("Rollout: unexpected error");
}

const SystemState* Rollout::getSystemState() const {
  return engine.getSystemState();
}

} // namespace BPMNOS::Rollout
