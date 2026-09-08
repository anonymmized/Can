#include <iostream>
#include <string>
#include <cstdlib>
#include "ServerManager.hpp"
#include "XrayConfigBuilder.hpp"
#include "XrayProcess.hpp"
#include "CliOptions.hpp"
#include "SystemCommand.hpp"
#include "TunManager.hpp"

int runConnection(Server server, ConnectionOptions options) {
    XrayConfigBuilder configBuilder;
    const auto executable = findExecutable(options.executable);
    if (options.tun) {
        TunManager tunManager;
        if (options.check) {
            const auto plan = tunManager.inspect(server, options.runtime.outboundInterface);
            auto runtime = options.runtime;
            runtime.outboundInterface = plan.outboundInterface;
            server.linkData.host = plan.serverAddress;
            configBuilder.buildTun(server, runtime, plan.tunInterface);
            std::cout << "TUN preflight passed. No network settings were changed.\n"
                        << "Xray: " << executable << '\n'
                        << "TUN: " << plan.tunInterface << '\n'
                        << "Outbound interface: " << plan.outboundInterface << '\n'
                        << "Server IP: " << plan.serverAddress << '\n';
            return 0;
        }
        return tunManager.run(server, options.runtime, executable);
    }
    if (options.runtime.outboundInterface.empty()) {
        options.runtime.outboundInterface = TunManager().socksOutboundInterface();
    }
    const auto config = configBuilder.build(server, options.runtime);
    requireFreeSocksPort(options.runtime);
    std::cout << "Starting SOCKS proxy at " << options.runtime.listenAddress << ':'
                 << options.runtime.socksPort << ". This is not system TUN mode.\n" << std::flush;
    if (!options.runtime.outboundInterface.empty()) {
        std::cout << "Outbound interface: " << options.runtime.outboundInterface << '\n' << std::flush;
    }
    return XrayProcess().run(config, {}, executable);
}

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "help"))) {
            printUsage();
            return 0;
        }
        ServerManager serverManager;
        if (argc == 4 && std::string(argv[1]) == "add") {
            serverManager.addServer(argv[2], argv[3]);
        } else if (argc == 3 && std::string(argv[1]) == "delete") {
            serverManager.deleteServer(parseServerNumber(argv[2]));
        } else if (argc == 2 && std::string(argv[1]) == "list") {
            serverManager.listServers();
            return 0;
        } else if (argc == 3 && std::string(argv[1]) == "show") {
            Server server = serverManager.getServer(parseServerNumber(argv[2]));
            std::cout << "Name: " << server.serverName << '\n';
            std::cout << "Host: " << server.linkData.host << '\n';
            std::cout << "Port: " << server.linkData.port << '\n';
            std::cout << "Security: " << server.linkData.security << '\n';
            std::cout << "Transport: " << server.linkData.transport << '\n';
        } else if (argc >= 3 && (std::string(argv[1]) == "config" || std::string(argv[1]) == "connect")) {
            const std::string command = argv[1];
            auto options = parseConnectionOptions(std::vector<std::string>(argv + 3, argv + argc));
            Server server = serverManager.getServer(parseServerNumber(argv[2]));
            XrayConfigBuilder builder;
            if (command == "config") {
                if (options.check) throw std::invalid_argument("--check is only available with connect");
                const auto config = options.tun
                    ? builder.buildTun(server, options.runtime, "utun100")
                    : builder.build(server, options.runtime);
                std::cout << config.dump(4) << '\n';
                return 0;
            }
            return runConnection(server, options);
        }
        else {
            std::cerr << "Unknown command or invalid arguments. Use can --help.\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
