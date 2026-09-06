#include <iostream>
#include <string>
#include "ServerManager.hpp"

int main(int argc, char** argv) {
    try {
        ServerManager serverManager;
        if (argc == 4 && std::string(argv[1]) == "add") {
            serverManager.addServer(argv[2], argv[3]);
        } else if (argc == 3 && std::string(argv[1]) == "delete") {
            serverManager.deleteServer(std::stoi(std::string(argv[2])));
        } else if (argc == 2 && std::string(argv[1]) == "list") {
            serverManager.listServers();
            return 0;
        } else if (argc == 3 && std::string(argv[1]) == "show") {
            Server server = serverManager.getServer(std::stoi(argv[2]));
            std::cout << "Name: " << server.serverName << '\n';
            std::cout << "Host: " << server.linkData.host << '\n';
            std::cout << "Port: " << server.linkData.port << '\n';
            std::cout << "Security: " << server.linkData.security << '\n';
            std::cout << "Transport: " << server.linkData.transport << '\n';
        } else {
            std::cerr << "Unknown command\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
