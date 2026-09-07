#include "CliOptions.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <net/if.h>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace {
unsigned int positiveNumber(const std::string& text, unsigned int maximum, const char* field) {
    unsigned int result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || result == 0 || result > maximum) {
        throw std::invalid_argument(std::string("Invalid ") + field + ": " + text);
    }
    return result;
}
}

int parseServerNumber(const std::string& text) {
    return static_cast<int>(positiveNumber(text, std::numeric_limits<int>::max(), "server number"));
}

ConnectionOptions parseConnectionOptions(const std::vector<std::string>& arguments) {
    ConnectionOptions options;
    if (const char* value = std::getenv("CAN_OUTBOUND_INTERFACE")) options.runtime.outboundInterface = value;
    if (const char* value = std::getenv("CAN_XRAY_BINARY"); value && *value) options.executable = value;
    bool portSpecified = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--tun") options.tun = true;
        else if (argument == "--check") options.check = true;
        else if (argument == "--interface" || argument == "--port" || argument == "--xray") {
            if (++index == arguments.size() || arguments[index].empty() || arguments[index].rfind("--", 0) == 0) {
                throw std::invalid_argument("Missing value for " + argument);
            }
            if (argument == "--interface") options.runtime.outboundInterface = arguments[index];
            else if (argument == "--xray") options.executable = arguments[index];
            else {
                options.runtime.socksPort = static_cast<std::uint16_t>(positiveNumber(arguments[index], 65535, "SOCKS port"));
                portSpecified = true;
            }
        } else {
            throw std::invalid_argument("Unknown option: " + argument);
        }
    }
    if (options.check && !options.tun) throw std::invalid_argument("--check requires --tun");
    if (options.tun && portSpecified) throw std::invalid_argument("--port is only available in SOCKS mode");
    return options;
}

void requireFreeSocksPort(const XrayRuntimeOptions& options) {
    if (!options.outboundInterface.empty() && ::if_nametoindex(options.outboundInterface.c_str()) == 0) {
        throw std::invalid_argument("Network interface not found: " + options.outboundInterface);
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.socksPort);
    if (::inet_pton(AF_INET, options.listenAddress.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("SOCKS listen address must be an IPv4 address");
    }
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == -1) throw std::system_error(errno, std::generic_category(), "Cannot check SOCKS port");
    const int result = ::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const int error = errno;
    ::close(socket);
    if (result != 0) {
        if (error == EADDRINUSE) {
            throw std::runtime_error("SOCKS port " + std::to_string(options.socksPort) +
                " is already in use. Stop the previous can/Xray process or use --port <number>.");
        }
        throw std::system_error(error, std::generic_category(), "Cannot bind SOCKS port");
    }
}

void printUsage() {
    std::cout <<
        "Usage:\n"
        "  can add <name> '<vless-url>'\n"
        "  can list\n"
        "  can show <id>\n"
        "  can delete <id>\n"
        "  can config <id> [--interface <name>] [--port <number>]\n"
        "  can config <id> --tun --interface <name>\n"
        "  can connect <id> [--interface <name>] [--port <number>]\n"
        "  can connect <id> --tun --check\n"
        "  sudo can connect <id> --tun\n"
        "  can connect <id> [--tun] --xray /absolute/path/to/xray\n\n"
        "Default: local SOCKS proxy; applications must use its proxy address.\n"
        "--tun: system IPv4/IPv6 TCP/UDP tunnel on macOS; requires root and no other active VPN.\n"
        "--check: read-only TUN preflight, without changing routes or DNS.\n"
        "Ctrl+C stops the connection. Server files are stored in ./data.\n";
}
