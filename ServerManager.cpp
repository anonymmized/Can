#include "ServerManager.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <stdexcept>

namespace {
void validateServerName(const std::string& name) {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") {
        throw std::invalid_argument("Invalid server name");
    }
    for (unsigned char character : name) {
        if (character < 32 || character == 127 || character == '/' || character == '\\') {
            throw std::invalid_argument("Server name must not contain path separators or control characters");
        }
    }
}
}

bool ServerManager::listCreated(const std::filesystem::path& listPath) {
    return std::filesystem::is_regular_file(listPath);
}

void ServerManager::createList(const std::filesystem::path& listPath) {
    std::filesystem::create_directories(listPath.parent_path());
    std::ofstream newFile(listPath);
    if (!newFile.is_open()) {
        throw std::runtime_error("Couldn't create list file: " + listPath.string());
    }
}

void ServerManager::loadList() {
    serversList.clear();
    std::filesystem::path listPath = LIST_PATH;
    if (!listCreated(listPath)) {
        createList(listPath);
    }

    std::ifstream fileWithList(listPath);
    if (!fileWithList.is_open()) {
        throw std::runtime_error("Couldn't open list file: " + listPath.string());
    }

    std::string line;
    while (std::getline(fileWithList, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        validateServerName(line);
        serversList.push_back(line);
    }

    if (fileWithList.bad()) {
        throw std::runtime_error("Error while reading list file: " + listPath.string());
    }
}

void ServerManager::saveList() {
    std::ofstream fileToSave(LIST_PATH, std::ios::trunc);
    if (!fileToSave.is_open()) {
        throw std::runtime_error("Couldn't open list file for writing");
    }
    for (const auto& server : serversList) {
        fileToSave << server << '\n';
    }
    fileToSave.close();
    if (!fileToSave) throw std::runtime_error("Couldn't finish writing the server list");
}

LinkData ServerManager::parseLink(const std::string& bareLink) {
    return linkParser.getParsedLink(bareLink);
}

void ServerManager::listServers() {
    loadList();
    if (serversList.empty()) {
        std::cout << "There is no servers in the list\n";
        return;
    }
    for (size_t serverIndex = 0; serverIndex < serversList.size(); serverIndex++) {
        std::cout << serverIndex + 1 << ". " << serversList[serverIndex] << '\n';
    }
}

void ServerManager::addServer(const std::string& serverName, const std::string& bareLink) {
    validateServerName(serverName);
    parseLink(bareLink);
    loadList();
    for (const auto& server : serversList) {
        if (server == serverName) {
            throw std::runtime_error("Server already exists");
        }
    }
    const auto serverPath = std::filesystem::path("data") / (serverName + ".txt");
    if (std::filesystem::exists(serverPath) || std::filesystem::is_symlink(serverPath)) {
        throw std::runtime_error("Server file already exists: " + serverPath.string());
    }
    std::ofstream fileToCreate(serverPath);
    if (!fileToCreate.is_open()) {
        throw std::runtime_error("Couldn't open list file to add server");
    }
    fileToCreate << bareLink;
    fileToCreate.close();
    if (!fileToCreate) throw std::runtime_error("Couldn't finish writing the server file");
    serversList.push_back(serverName);
    saveList();
}

void ServerManager::deleteServer(int serverNum) {
    loadList();
    if (serverNum < 1 || static_cast<std::size_t>(serverNum) > serversList.size()) {
        throw std::out_of_range("Invalid server number");
    }
    std::string serverName = serversList[serverNum - 1];
    std::filesystem::path serverPath = std::filesystem::path("data") / (serverName + ".txt");
    if (!std::filesystem::remove(serverPath)) {
        throw std::runtime_error("Server file not found");
    }
    serversList.erase(serversList.begin() + serverNum - 1);
    saveList();
}

Server ServerManager::getServer(int serverNum) {
    loadList();
    if (serverNum < 1 || static_cast<size_t>(serverNum) > serversList.size()) {
        throw std::out_of_range("Invalid server number");
    }
    Server targetServer;
    targetServer.serverName = serversList[serverNum - 1];
    std::filesystem::path serverPath = std::filesystem::path("data") / (targetServer.serverName + ".txt");
    std::ifstream targetServerFile(serverPath);
    if (!targetServerFile.is_open()) {
        throw std::runtime_error("Server file not found: " + serverPath.string());
    }
    std::string bareUrl;
    if (!std::getline(targetServerFile, bareUrl)) {
        throw std::runtime_error("Couldn't read server file: " + serverPath.string());
    }
    targetServer.linkData = linkParser.getParsedLink(bareUrl);
    return targetServer;
}
