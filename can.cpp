#include <iostream>
#include <string>
#include "Server.hpp"

int main(int argc, char** argv) {
    ServerManager serverManager;
    if (argc == 4 && std::string(argv[1]) == "add") {
        serverManager.addServer(std::string(argv[2]), std::string(argv[3]));
    } else if (argc == 3 && std::string(argv[1]) == "delete") {
        serverManager.deleteServer(std::stoi(std::string(argv[2])));
    }
    serverManager.listServers();
    return 0;
}
