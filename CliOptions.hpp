#pragma once

#include "XrayConfigBuilder.hpp"

#include <string>
#include <vector>

struct ConnectionOptions {
    bool tun = false;
    bool check = false;
    XrayRuntimeOptions runtime;
    std::string executable = "xray";
};

int parseServerNumber(const std::string& text);
ConnectionOptions parseConnectionOptions(const std::vector<std::string>& arguments);
void requireFreeSocksPort(const XrayRuntimeOptions& options);
void printUsage();
