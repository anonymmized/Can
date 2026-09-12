#pragma once

#include <string>
#include <filesystem>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <system_error>

class BackgroundSession {
    private:
        const std::string pathToSession = "/var/run/can.session";
        bool needToStop = false;
        pid_t backgroundCanPid = -1;
        std::string serverName;
        bool tunOrSocks;
        std::filesystem::path pathToLog;
        int sessionFd = -1;
        void validateAndLockSessionFile();
        void checkSessionFile();
        void lockSessionFile();
        void writeInitialState();
    public:
        BackgroundSession(std::string _serverName, bool _tunOrSocks, std::filesystem::path _pathToLog) : serverName(_serverName), tunOrSocks(_tunOrSocks), pathToLog(_pathToLog) {
            backgroundCanPid = getpid();
            sessionFd = open(pathToSession.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (sessionFd == -1) {
                throw std::system_error(errno, std::generic_category(), "Cannot open session file");
            }
            validateAndLockSessionFile();
        }
        BackgroundSession(const BackgroundSession&) = delete;
        BackgroundSession& operator=(const BackgroundSession&) = delete;
        ~BackgroundSession() {
            if (sessionFd != -1) {
                close(sessionFd);
            }
        }
        static std::string status();
};
