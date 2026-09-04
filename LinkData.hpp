#pragma once

#include <string>

struct LinkData {
    std::string uuid;
    std::string host;
    std::uint16_t port{};

    std::string encryption;
    std::string flow;
    std::string security;
    std::string sni;
    std::string fingerprint;
    std::string publicKey;
    std::string shortId;
    std::string transport;

    std::string rawUrl;
};
