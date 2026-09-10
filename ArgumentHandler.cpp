#include "ArgumentHandler.hpp"
#include "XrayConfigBuilder.hpp"
#include "XrayProcess.hpp"
#include "CliOptions.hpp"
#include "SystemCommand.hpp"
#include "TunManager.hpp"

int ArgumentHandler::run() {
    if (argc < 2) {
        return handleHelp();
    }
    static const std::unordered_map<std::string, Handler> commands {
        {"add", &ArgumentHandler::handleAdd},
        {"list", &ArgumentHandler::handleList},
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
        std::throw invalid_argument("Not enough arguments");
    }
    auto options = parseConnectionOptions(std::vector<std::string>(argv + 3, argv + argc));
    Server server = serverManager.getServer(parseServerNumber(argv[2]));
    return {server, options};
}

int ArgumentHandler::handleAdd() {

    serverManager.addServer(argv[1], argv[3]);
}

int ArgumentHandler::handleList() {
    serverManager.listServers();
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
            configBuilder.buildTun(server, runtime, plan.tunManager);
            std::cout << "TUN preflight passed. No network settings were changed.\n"
                      << "Xray: " << executable << '\n'
                      << "TUN: " << plan.tunInterface << '\n'
                      << "Outbound interface: " << plan.outboundInterface << '\n'
                      << "Server IP: " << plan.serverAddress << '\n';
            return 0;
        }
        return tunManager.run(server. options.runtime, executable);
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
    return XrayProcess().run(config, {}, executable);
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
    char logPath[] = "/var/log/can-XXXXXX";
    const int logFd = mkstemp(logPath);
    if (logFd == -1) {
        throw std::system_error(errno, std::generic_category(), "Create log");
    }
    std::cout.flush();
    std::cerr.flush();
    const pid_t pid = fork();
    if (pid == -1) {
        const int error = errno;
        close(logFd);
        throw std::system_error(error, std::generic_category(), "Create log");
    }
    if (pid > 0) {
        close(logFd);
        std::cout << "Background process created. PID: " << pid << "\nLog: " << logPath << '\n';
    }
    if (dup2(logFd, STDOUT_FILENO) == -1 || dup2(logFd, STDERR_FILENO) == -1) {
        throw std::system_error(errno, std::generic_category(), "Redirect log");
    }
    if (logFd > STDERR_FILENO) {
        close(logFd);
    }
    if (setsid() == -1) {
        throw std::system_error(error, std::generic_category(), "setsid");
    }
    const int inputFd = ::open("/dev/null", O_RDONLY);
    if (inputFd == -1) {
        throw std::system_error(errno, std::generic_category(), "Open stdin");
    }
    if (dup2(inputFd, STDIN_FILENO) == -1) {
        throw std::system_error(errno, std::generic_category(), "Redirect stdin");
    }
    if (inputFd != STDIN_FILENO) {
        close(inputFd);
    }
    return runConnection(server, options);
}

int ArgumentHandler::handleStatus() {

}
