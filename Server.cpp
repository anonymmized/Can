#include "Server.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>

void ServerManager::loadList() {
    serversList.clear();
    std::filesystem::path listPath = LIST_PATH;
    std::filesystem::create_directories(listPath.parent_path());
    if (!std::filesystem::exists(listPath)) {
        std::ofstream newFile(listPath);
        if (!newFile.is_open()) {
            std::cerr << "Couldn't create list file\n";
        }
        return;
    }
    std::ifstream fileWithList(listPath);
    if (!fileWithList.is_open()) {
        std::cerr << "File with list wasn't opened\n";
        return;
    }
    std::string line;
    while (std::getline(fileWithList, line)) {
        serversList.push_back(line);
    }
}

void ServerManager::saveList() {
    std::ofstream fileToSave(LIST_PATH, std::ios::trunc);
    if (!fileToSave.is_open()) {
        std::cerr << "File to save wasn't opened\n";
        return;
    }
    for (const auto& server : serversList) {
        fileToSave << server << '\n';
    }
}

void ServerManager::listServers() {
    loadList();
    for (size_t i = 0; i < serversList.size(); i++) {
        std::cout << i + 1 << ". " << serversList[i] << '\n';
    }
}

void ServerManager::addServer(const std::string& serverName, const std::string& bareLink) {
    std::ofstream fileToCreate("data/" + serverName + ".txt", std::ios::trunc);
    if (!fileToCreate.is_open()) {
        std::cerr << "File to create wasn't opened\n";
        return;
    }
    fileToCreate << serverName << " - " << bareLink;
    loadList();
    serversList.push_back(serverName);
    saveList();
}

void ServerManager::deleteServer(int serverNum) {
    loadList();
    std::string serverName = serversList[serverNum - 1];
    try {
        if (std::filesystem::remove(serverName + ".txt")) {
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
