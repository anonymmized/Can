#include "BackgroundSession.hpp"

#include <sys/stat.h>
#include <sys/file.h>
#include <stdexcept>

namespace {
    class StateWriteLock {
        private:
            int fd;
        public:
            explicit StateWriteLock(int descriptor) : fd(descriptor) {
                struct flock lock{};
                lock.l_type = F_WRLCK;
                lock.l_whence = SEEK_SET;
                lock.l_start = 0;
                lock.l_len = 1;
                int result;
                do {
                    result = fcntl(fd, F_SETLKW, &lock);
                } while (result == -1 && errno == EINTR);
                if (result == -1) {
                    throw std::system_error(errno, std::generic_category(), "Cannot lock session update");
                }
            }
            StateWriteLock(const StateWriteLock&) = delete;
            StateWriteLock& operator=(const StateWriteLock&) = delete;
            ~StateWriteLock() {
                struct flock lock{};
                lock.l_type = F_UNLCK;
                lock.l_whence = SEEK_SET;
                lock.l_start = 0;
                lock.l_len = 1;
                fcntl(fd, F_SETLK, &lock);
            }
    };
};

void BackgroundSession::lockSessionFile() {
    int result;
    do {
        result = flock(sessionFd, LOCK_EX | LOCK_NB);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            throw std::runtime_error("Another background Can session is already running");
        }
        throw std::system_error(errno, std::generic_category(), "Cannot lock session file");
    }
}

void BackgroundSession::checkSessionFile() {
    struct stat info{};
    if (fstat(sessionFd, &info) == -1) {
        throw std::system_error(errno, std::generic_category(), "Cannot inspect session file");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || (info.st_mode & 077) != 0) {
        throw std::runtime_error("Unsafe session file");
    }
}

void BackgroundSession::validateAndLockSessionFile() {
    try {
        checkSessionFile();
        StateWriteLock updateLock(sessionFd);
        lockSessionFile();
        writeInitialState();
    } catch (...) {
        close(sessionFd);
        sessionFd = -1;
        throw;
    }
}

void BackgroundSession::writeInitialState() {
    const std::string text = "0\nPID: " + std::to_string(backgroundCanPid) +
                             "\nServer: " + serverName + 
                             "\nMode: " + (tunOrSocks ? "TUN" : "SOCKS") + 
                             "\nLog: " + pathToLog.string() + "\n";
    if (ftruncate(sessionFd, 0) == -1) {
        throw std::system_error(errno, std::generic_category(), "Cannot clear session file");
    }
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = pwrite(sessionFd, text.data() + written, text.size() - written, static_cast<off_t>(written));
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "Cannot write session");
        }
        if (count == 0) {
            throw std::runtime_error("Session write made no progress");
        }
        written += static_cast<size_t>(count);
    }
}

std::string BackgroundSession::status() {
    const int fd = open(pathToSession.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd == -1) {
        if (errno == ENOENT) {
            return "No active background session\n";
        }
        throw std::system_error(errno, std::generic_category(), "Cannot open session file");
    }
    struct FileGuard {
        int fd;
        ~FileGuard() {
            close(fd);
        }
    } guard{fd};
    struct stat info{};
    if (fstat(fd, &info) == -1) {
        throw std::system_error(errno, std::generic_category(), "Cannot inspect session file");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || (info.st_mode & 077) != 0) {
        throw std::runtime_error("Unsage session file");
    }
    int result;
    do {
        result = flock(fd, LOCK_EX | LOCK_NB);
    } while (result == -1 && errno == EINTR);
    if (result == 0) {
        return "No active background session\n";
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        throw std::system_error(errno, std::generic_category(), "Cannot checl session lock");
    }
    std::string text;
    char buffer[512];
    off_t offset = 0;
    while (true) {
        const ssize_t count = pread(fd, buffer, sizeof(buffer), offset);
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "Cannot read session file");
        }
        if (count == 0) {
            break;
        }
        text.append(buffer, static_cast<size_t>(count));
        offset += static_cast<off_t>(count);
        if (text.size() > 4096) {
            throw std::runtime_error("Session file is too large");
        }
    }
    if (text.size() < 2) {
        return "Background session is initializing\n";
    }
    if ((text[0] != '0' && text[0] != '1') || text[1] != '\n') {
        throw std::runtime_error("Invalid session file format");
    }
    const std::string state = text[0] == '1' ? "Stop requested\n" : "Background session active; connectivity not checked\n";
    return state + text.substr(2);
}

bool BackgroundSession::hasStopRequest() noexcept {
    if (activeSession == nullptr) {
        return false;
    }
    if (activeSession->needToStop) {
        return true;
    }
    char flag = '\0';
    ssize_t count;
    do {
        count = pread(activeSession->sessionFd, &flag, 1, 0);
    } while (count == -1 && errno == EINTR);
    activeSession->needToStop = count != 1 || flag != '0';
    return activeSession->needToStop;
}

void BackgroundSession::requestStop() {
    const int fd = open(pathToSession.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd == -1) {
        if (errno == ENOENT) {
            throw std::runtime_error("No active background session");
        }
        throw std::system_error(errno, std::generic_category(), "Cannot open session file");
    }
    struct FileGuard {
        int fd; 
        ~FileGuard() { close(fd); }
    } guard{fd};
    struct stat info{};
    if (fstat(fd, &info) == -1) {
        throw std::system_error(errno, std::generic_category(), "Cannot inspect session file");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || (info.st_mode & 077) != 0) {
        throw std::runtime_error("Unsafe session file");
    }
    StateWriteLock updateLock(fd);
    int result;
    do {
        result = flock(fd, LOCK_EX | LOCK_NB);
    } while (result == -1 && errno == EINTR);
    if (result == 0) {
        throw std::runtime_error("No active background session");
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        throw std::system_error(errno, std::generic_category(), "Cannot check session lock");
    }
    ssize_t count;
    do {
        count = pwrite(fd, "1", 1, 0);
    } while (count == -1 && errno == EINTR);
    if (count == -1) {
        throw std::system_error(errno, std::generic_category(), "Cannot write stop request");
    }
    if (count != 1) {
        throw std::runtime_error("Stop request was not written");
    }
}
