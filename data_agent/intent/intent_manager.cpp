#include "data_agent/intent/intent_manager.h"

namespace data_agent {

IntentManager::IntentManager() {}

void IntentManager::Reset() {
    intents_.clear();
}

void IntentManager::Add(const Intent &intent) {
    intents_.push_back(intent);
}

const std::vector<Intent> &IntentManager::intents() const {
    return intents_;
}

}  // namespace data_agent
