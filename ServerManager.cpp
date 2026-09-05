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

void ServerManager::printParsedLinkParts(const std::string& bareLink) {
    LinkData linkData = linkParser.getParsedLink(bareLink);
    std::cout << "Uuid: " << linkData.uuid << '\n';
    std::cout << "Host: " << linkData.host << '\n';
    std::cout << "Port: " << linkData.port << '\n';
    std::cout << "Encryption: " << linkData.encryption << '\n';
    std::cout << "Flow: " << linkData.flow << '\n';
    std::cout << "Security: " << linkData.security << '\n';
    std::cout << "Sni: " << linkData.sni << '\n';
    std::cout << "Fingerprint: " << linkData.fingerprint << '\n';
    std::cout << "Public Key: " << linkData.publicKey << '\n';
    std::cout << "Short ID: " << linkData.shortId << '\n';
    std::cout << "Transport: " << linkData.transport << '\n';
    std::cout << '\n' << bareLink << '\n';
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
    std::ofstream fileToCreate("data/" + serverName + ".txt", std::ios::trunc);
    if (!fileToCreate.is_open()) {
        throw std::runtime_error("Couldn't open list file to add server");
    }
    fileToCreate << serverName << " - " << bareLink;
    loadList();
    serversList.push_back(serverName);
    saveList();
}

void ServerManager::deleteServer(int serverNum) {
    loadList();
    if (serverNum < 1 || static_cast<std::size_t>(serverNum) > serversList.size()) {
        throw std::out_of_range("Invalid server number");
    }
    std::string serverName = serversList[serverNum - 1];

    try {
        if (std::filesystem::remove("data/" + serverName + ".txt")) {
            std::cout << "Server " << serverName << " removed\n";
        } else {
            std::cout << "Server don't found\n";
        }
    } catch (const std::filesystem::filesystem_error& error) {
        std::cerr << "Error: " << error.what() << '\n';
    }
    serversList.erase(serversList.begin() + serverNum - 1);
    saveList();
}
