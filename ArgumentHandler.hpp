#pragma once

#include <string>
#include <unordered_map>
#include <stdexcept>
#include "ServerManager.hpp"
#include "CliOptions.hpp"

class ArgumentHandler {
    private:
        using Handler = int(ArgumentHandler::*)();
        int argc;
        char* const* argv;
        ServerManager serverManager;
        int runConnection(Server server, ConnectionOptions options);
        std::pair<Server, ConnectionOptions> prepareServerAndOptions();
        int handleAdd();
        int handleList();
        int handleConfig();
        int handleConnect();
        int handleQuickrun();
        int handleStatus();
        int handleStop();
        int handleHelp();
    public:
        ArgumentHandler(int _argc, char** _argv) : argc(_argc), argv(_argv) {}
        int run();
};
