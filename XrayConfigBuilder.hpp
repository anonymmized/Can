#pragma once

#include "ServerManager.hpp"

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

struct XrayRuntimeOptions {
    std::string listenAddress = "127.0.0.1";
    std::uint16_t socksPort = 1080;
    std::string logLevel = "warning";
    std::string outboundInterface;
};

class XrayConfigBuilder {
    private:
        void validate(const Server& server) const;
        nlohmann::json buildInbound(const XrayRuntimeOptions& options) const;
        nlohmann::json buildOutbound(const LinkData& linkData) const;
    public:
        nlohmann::json build(const Server& server, const XrayRuntimeOptions& options) const;
        nlohmann::json buildTun(const Server& server, const XrayRuntimeOptions& options,
                               const std::string& tunName) const;
};
