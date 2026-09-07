#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <string>

struct XrayProcessHooks {
    std::function<bool()> ready;
    std::function<void()> onReady;
    std::function<void()> onStop;
};

class XrayProcess {
    public:
        int run(const nlohmann::json& config, const XrayProcessHooks& hooks = {},
                const std::string& executable = "xray") const;
        static bool stopRequested();
};
