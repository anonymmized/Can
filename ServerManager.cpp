#include "ServerManager.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <stdexcept>

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
    LinkData parsedLink = parseLink(bareLink);
    loadList();
    for (const auto& server : serversList) {
        if (server == serverName) {
            throw std::runtime_error("Server already exists");
        }
    }
    std::ofstream fileToCreate("data/" + serverName + ".txt", std::ios::trunc);
    if (!fileToCreate.is_open()) {
        throw std::runtime_error("Couldn't open list file to add server");
    }
    fileToCreate << bareLink;
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
