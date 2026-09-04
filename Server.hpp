#pragma once

#include <string>
#include <vector>
#include <filesystem>

struct LinkData {
    std::string protocol;
    std::string uuid;
    std::string host;
    int port;

    std::string security;
    std::string publicKey;
    std::string shortId;
    std::string serverName;
};

struct Server {
    std::string serverName;
    LinkData linkData;
};

const std::string LIST_PATH = "./data/list.txt";

class ServerManager {
    private:
        std::vector<std::string> serversList;
        bool listCreated(const std::filesystem::path& listPath);
        void createList(const std::filesystem::path& listPath);
        void loadList();
        void saveList();

        LinkData parseLink(const std::string& link);
    public:
        void addServer(const std::string& serverName, const std::string& bareLink);
        void deleteServer(int serverNum);
        Server getServer(int serverNum);
        void listServers();
};
