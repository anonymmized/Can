#include "XrayConfigBuilder.hpp"

#include <stdexcept>

nlohmann::json XrayConfigBuilder::buildInbound(const XrayRuntimeOptions& options) const {
    if (options.listenAddress.empty()) {
        throw std::invalid_argument("Inbound listen address is empty");
    } 
    if (options.socksPort == 0) {
        throw std::invalid_argument("Inbound SOCKS port cannot be 0");
    }
    return {
        {"tag", "socks-in"},
        {"listen", options.listenAddress},
        {"port", options.socksPort},
        {"protocol", "socks"},
        {"settings", {{"auth", "noauth"}, {"udp", true}}}
    };
}

nlohmann::json XrayConfigBuilder::buildOutbound(const LinkData& linkData) const {
    if (linkData.transport != "tcp" && linkData.transport != "raw") {
        throw std::invalid_argument("Only TCP/raw transport is currently supported");
    }
    if (linkData.security != "reality") {
        throw std::invalid_argument("Only REALITY security is currently supported");
    }
    nlohmann::json userSettings = {
        {"id", linkData.uuid},
        {"encryption", linkData.encryption.empty() ? "none" : linkData.encryption}
    };
    if (!linkData.flow.empty()) {
        userSettings["flow"] = linkData.flow;
    }
    nlohmann::json serverSettings = {
        {"address", linkData.host},
        {"port", linkData.port},
        {"users", nlohmann::json::array({std::move(userSettings)})}
    };
    nlohmann::json vlessSettings = {
        {"vnext", nlohmann::json::array({std::move(serverSettings)})}
    };
    nlohmann::json realitySettings = {
        {"fingerprint", linkData.fingerprint.empty() ? "chrome" : linkData.fingerprint},
        {"publicKey", linkData.publicKey},
        {"serverName", linkData.sni},
        {"shortId", linkData.shortId},
        {"spiderX", ""}
    };
    return {
        {"protocol", "vless"},
        {"settings", std::move(vlessSettings)},
        {"streamSettings", {
            {"network", "tcp"},
            {"security", "reality"},
            {"realitySettings", std::move(realitySettings)}
        }},
        {"tag", "proxy"}
    };
}

nlohmann::json XrayConfigBuilder::build(const Server& server, const XrayRuntimeOptions& options) const {
    validate(server);

    nlohmann::json config = {
        {"log", {{"loglevel", options.logLevel}}},
        {"inbounds", nlohmann::json::array({buildInbound(options)})},
        {"outbounds", nlohmann::json::array({buildOutbound(server.linkData)})}
    };
    if (!options.outboundInterface.empty()) {
        config["outbounds"][0]["streamSettings"]["sockopt"]["interface"] = options.outboundInterface;
    }
    return config;
}

nlohmann::json XrayConfigBuilder::buildTun(const Server& server, const XrayRuntimeOptions& options, const std::string& tunName) const {
    validate(server);
    if (tunName.empty() || options.outboundInterface.empty()) {
        throw std::invalid_argument("TUN requires a name and a physical outbound interface");
    }
    auto tunData = server.linkData;
    if (tunData.flow == "xtls-rprx-vision") tunData.flow = "xtls-rprx-vision-udp443";
    auto outbound = buildOutbound(tunData);
    outbound["streamSettings"]["sockopt"]["interface"] = options.outboundInterface;
    return {
        {"log", {{"loglevel", options.logLevel}}},
        {"inbounds", nlohmann::json::array({{
            {"tag", "tun-in"}, {"protocol", "tun"},
            {"settings", {{"name", tunName}, {"MTU", 1500}, {"userLevel", 0}}}
        }})},
        {"outbounds", nlohmann::json::array({std::move(outbound)})}
    };
}

void XrayConfigBuilder::validate(const Server& server) const {
    const LinkData& data = server.linkData;
    if (data.uuid.empty()) {
        throw std::invalid_argument("UUID is empty");
    }

    if (data.host.empty()) {
        throw std::invalid_argument("Server host is empty");
    }

    if (data.port == 0) {
        throw std::invalid_argument("Server port cannot be 0");
    }

    if (data.sni.empty()) {
        throw std::invalid_argument("REALITY SNI is empty");
    }

    if (data.publicKey.empty()) {
        throw std::invalid_argument("REALITY public key is empty");
    }
}
