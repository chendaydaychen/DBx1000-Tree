#pragma once

#include <vector>

#include "data_agent/intent/intent.h"

namespace data_agent {

class IntentManager {
public:
    IntentManager();

    void Reset();
    void Add(const Intent &intent);
    const std::vector<Intent> &intents() const;

private:
    std::vector<Intent> intents_;
};

}  // namespace data_agent
