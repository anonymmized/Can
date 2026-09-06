#pragma once

#include <nlohmann/json.hpp>

class XrayProcess {
    public:
        int run(const nlohmann::json& config) const;
};
