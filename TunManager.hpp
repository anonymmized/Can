#pragma once

#include "XrayConfigBuilder.hpp"

struct TunPlan {
    std::string tunInterface;
    std::string outboundInterface;
    std::string serverAddress;
    std::string serverGateway;
};

class TunManager {
public:
    TunPlan inspect(const Server& server, const std::string& requestedInterface = "") const;
    std::string socksOutboundInterface() const;
    int run(const Server& server, XrayRuntimeOptions options,
            const std::string& executable = "xray") const;
};
