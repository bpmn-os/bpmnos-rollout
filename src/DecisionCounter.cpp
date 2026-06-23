#include "DecisionCounter.h"

using namespace BPMNOS::Rollout;
using namespace BPMNOS::Execution;

void DecisionCounter::connect(Notifier* notifier) {
  notifier->addSubscriber(this, Observable::Type::Event);
}

const std::size_t& DecisionCounter::count() const {
  return decisions;
}

void DecisionCounter::notice(const Observable* observable) {
  if ( auto decision = dynamic_cast<const Decision*>(observable);
       decision && isCounted(decision) ) {
    ++decisions;
  }
}

bool DecisionCounter::isCounted(const Decision* decision) {
  if ( dynamic_cast<const EntryDecision*>(decision) ) {
    // only entries into the children of a sequential ad-hoc subprocess are rolled out
    auto node = decision->token->node;
    return node->parent && node->parent->represents<BPMNOS::Model::SequentialAdHocSubProcess>();
  }
  if ( dynamic_cast<const ExitDecision*>(decision) ) {
    return false;                                   // exits are always dispatched greedily
  }
  if ( dynamic_cast<const ChoiceDecision*>(decision) ) {
    return true;                                    // every choice is rolled out
  }
  if ( auto messageDelivery = dynamic_cast<const MessageDeliveryDecision*>(decision) ) {
    auto message = messageDelivery->message.lock();
    if ( !message ) {
      return false;
    }
    // Direct (explicitly addressed) deliveries are dispatched greedily by InstantDirectMessage; the rest are
    // rolled out. Direct test replicated from InstantDirectMessage on the recipient token.
    auto token = decision->token;
    auto extensionElements = token->node->extensionElements->as<BPMNOS::Model::ExtensionElements>();
    auto messageDefinition = extensionElements->getMessageDefinition(token->status);
    auto recipientHeader = messageDefinition->getRecipientHeader(token->getAttributeRegistry(), token->status, *token->data, token->globals);
    bool direct = message->recipient.has_value() || recipientHeader[BPMNOS::Model::MessageDefinition::Index::Sender].has_value();
    return !direct;
  }
  return false;
}
