#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include "LinkParser.hpp"
#include "LinkData.hpp"

struct Server {
    std::string serverName;
    LinkData linkData;
};

const std::string LIST_PATH = "./data/list.txt";

class ServerManager {
    private:
        std::vector<std::string> serversList;
        LinkParser linkParser;
        bool listCreated(const std::filesystem::path& listPath);
        void createList(const std::filesystem::path& listPath);
        void loadList();
        void saveList();

    public:
        void addServer(const std::string& serverName, const std::string& bareLink);
        void deleteServer(int serverNum);
        Server getServer(int serverNum);
        void listServers();
        void printParsedLinkParts(const std::string& bareLink);
};
