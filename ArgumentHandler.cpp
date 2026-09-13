#include "ArgumentHandler.hpp"
#include "BackgroundLauncher.hpp"
#include "StatusRenderer.hpp"
#include "BackgroundSession.hpp"
#include "XrayConfigBuilder.hpp"
#include "XrayProcess.hpp"
#include "CliOptions.hpp"
#include "SystemCommand.hpp"
#include "TunManager.hpp"

#include <iostream>
#include <unistd.h>

int ArgumentHandler::run() {
    if (argc < 2) {
        return handleHelp();
    }
    static const std::unordered_map<std::string, Handler> commands {
        {"add", &ArgumentHandler::handleAdd},
        {"list", &ArgumentHandler::handleList},
        {"show", &ArgumentHandler::handleShow},
        {"delete", &ArgumentHandler::handleDelete},
        {"config", &ArgumentHandler::handleConfig},
        {"connect", &ArgumentHandler::handleConnect},
        {"quickrun", &ArgumentHandler::handleQuickrun},
        {"status", &ArgumentHandler::handleStatus},
        {"stop", &ArgumentHandler::handleStop},
        {"help", &ArgumentHandler::handleHelp},
        {"--help", &ArgumentHandler::handleHelp}
    };
    const auto it = commands.find(argv[1]);
    if (it == commands.end()) {
        throw std::invalid_argument("Unknown command");
    }
    return (this->*(it->second))();
}

std::pair<Server, ConnectionOptions> ArgumentHandler::prepareServerAndOptions() {
    if (argc < 3) {
        handleHelp();
        throw std::invalid_argument("A server number is required");
    }
    auto options = parseConnectionOptions(std::vector<std::string>(argv + 3, argv + argc));
    Server server = serverManager.getServer(parseServerNumber(argv[2]));
    return {server, options};
}

int ArgumentHandler::handleAdd() {
    if (argc != 4) {
        throw std::invalid_argument("Usage: can add <name> '<vless-url>'");
    }
    serverManager.addServer(argv[2], argv[3]);
    return 0;
}

int ArgumentHandler::handleList() {
    if (argc != 2) {
        throw std::invalid_argument("Usage: can list");
    }
    serverManager.listServers();
    return 0;
}

int ArgumentHandler::handleShow() {
    if (argc != 3) throw std::invalid_argument("Usage: can show <id>");
    const auto server = serverManager.getServer(parseServerNumber(argv[2]));
    std::cout << "Name: " << server.serverName << '\n'
              << "Host: " << server.linkData.host << '\n'
              << "Port: " << server.linkData.port << '\n'
              << "Security: " << server.linkData.security << '\n'
              << "Transport: " << server.linkData.transport << '\n';
    return 0;
}

int ArgumentHandler::handleDelete() {
    if (argc != 3) throw std::invalid_argument("Usage: can delete <id>");
    serverManager.deleteServer(parseServerNumber(argv[2]));
    return 0;
}

int ArgumentHandler::runConnection(Server server, ConnectionOptions options) {
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
              << options.runtime.socksPort << ". This is not system TUN mode\n" << std::flush;
    if (!options.runtime.outboundInterface.empty()) {
        std::cout << "Outbound interface:" << options.runtime.outboundInterface << '\n' << std::flush;
    }
    XrayProcessHooks hooks;
    hooks.ready = [&] { return socksListenerReady(options.runtime); };
    return XrayProcess().run(config, hooks, executable);
}

int ArgumentHandler::handleConfig() {
    auto [server, options] = prepareServerAndOptions();
    XrayConfigBuilder builder;
    if (options.check) {
        throw std::invalid_argument("--check is only available with connect");
    }
    const auto config = options.tun ? builder.buildTun(server, options.runtime, "utun100") : builder.build(server, options.runtime);
    std::cout << config.dump(4) << '\n';
    return 0;
}

int ArgumentHandler::handleConnect() {
    auto [server, options] = prepareServerAndOptions();
    return runConnection(server, options);
}

int ArgumentHandler::handleQuickrun() {
    auto [server, options] = prepareServerAndOptions();
    if (options.check) {
        throw std::invalid_argument("Use connect --tun --check");
    }
    if (::geteuid() != 0)
        throw std::runtime_error("quickrun uses /var/run and /var/log. Run it with sudo; use sudo for status and stop too.");
    options.executable = findExecutable(options.executable);
    const std::string message = options.tun ? "TUN started in background." :
        "SOCKS proxy started in background. This is not system VPN mode; use --tun for that.";
    return BackgroundLauncher::run("/var/log", message,
        [this, server = server, options = options](const std::filesystem::path& logPath,
                                                   const BackgroundLauncher::Ready& ready) {
            BackgroundSession session(server.serverName, options.tun, logPath,
                                      BackgroundSession::defaultPath(), ready);
            return runConnection(server, options);
        });
}

int ArgumentHandler::handleStatus() {
    if (argc != 2) {
        throw std::invalid_argument("Usage: can status");
    }
    if (::geteuid() != 0)
        throw std::runtime_error("Background sessions are owned by root. Use sudo can status.");
    const bool color = ::isatty(STDOUT_FILENO) != 0;
    std::cout << StatusRenderer::render(BackgroundSession::status(), color);
    return 0;
}

int ArgumentHandler::handleStop() {
    if (argc != 2) {
        throw std::invalid_argument("Usage: can stop");
    }
    if (::geteuid() != 0)
        throw std::runtime_error("Background sessions are owned by root. Use sudo can stop.");
    BackgroundSession::requestStop();
    std::cout << "Background session ended. Check its log for cleanup errors.\n";
    return 0;
}

int ArgumentHandler::handleHelp() {
    printUsage();
    return 0;
}
