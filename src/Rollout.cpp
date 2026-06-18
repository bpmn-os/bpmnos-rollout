#include "Rollout.h"
#include <stdexcept>
#include <cassert>

using namespace BPMNOS::Execution;
using namespace BPMNOS::Model;

namespace BPMNOS::Rollout {

Rollout::Rollout( const std::shared_ptr<Decision>& decision, const SystemState* systemState, Evaluator* evaluator, unsigned int index )
  : scenario(forkScenario(systemState, index))
  , systemState(std::make_unique<SystemState>( &engine, scenario, systemState ))
  , evaluator(evaluator)
  , decision(cloneDecision(decision))
{
  // TODO: run engine with selected decision
}

const BPMNOS::Model::Scenario* Rollout::forkScenario(const BPMNOS::Execution::SystemState* systemState, unsigned int index) {
  if ( auto* stochasticScenario = dynamic_cast<const StochasticScenario*>(systemState->scenario) ) {
    // Fork at spawnTime so future values resample independently; offset the seed by the repetition index
    // so the same index yields the same resampled future across candidates (common random numbers).
    unsigned int seed = stochasticScenario->getSeed() + index;
    forkedScenario = std::make_unique<StochasticScenario>( const_cast<StochasticScenario*>(stochasticScenario), systemState->getTime() + 1, seed );
    return forkedScenario.get();
  }
  // For deterministic scenarios, just return the original pointer to be re-used.
  return systemState->scenario;
}


std::shared_ptr<BPMNOS::Execution::Decision> Rollout::cloneDecision( const std::shared_ptr<BPMNOS::Execution::Decision>& original ) {
  // The token is unambiguously identified by its instance identifier and node, both stable across the copy.
  const Token* originalToken = original->token;
  auto instanceId = originalToken->getInstanceId();
  const BPMN::FlowNode* node = originalToken->node;

  // Find the equivalent token in a pending-decision list of the copied state.
  auto findToken = [instanceId, node]( auto& pending ) -> const BPMNOS::Execution::Token* {
    for ( auto& [token_ptr,_] : pending ) {
      if ( auto token = token_ptr.lock() ) {
        if ( token->getInstanceId() == instanceId && token->node == node ) {
          return token.get();
        }
      }
    }
    throw std::logic_error("Rollout: cannot find token at node '" + node->id + "' for instance '" + BPMNOS::to_string(instanceId, STRING) + "'");
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
      for ( auto& message : systemState->messages ) {
        if ( message->state == Message::State::CREATED && message->origin == origin
          && message->header[BPMNOS::Model::MessageDefinition::Index::Sender] == sender )
        {
          return std::make_shared<MessageDeliveryDecision>(token, message.get(), evaluator);
        }
      }
      throw std::logic_error("Rollout: cannot find message with origin '" + origin->id + "' sent from '" + BPMNOS::to_string(sender.value(), STRING) + "'");
    }
  }
  throw std::logic_error("Rollout: unexpected error");
}

const SystemState* Rollout::getSystemState() const {
  return systemState.get();
}

} // namespace BPMNOS::Rollout
