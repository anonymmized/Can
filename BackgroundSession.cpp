#include "BackgroundSession.hpp"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
struct FileGuard {
    int fd;
    explicit FileGuard(int value) : fd(value) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    ~FileGuard() { if (fd >= 0) ::close(fd); }
    int release() { return std::exchange(fd, -1); }
};

int openChecked(const std::filesystem::path& path, bool create) {
    const int flags = O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (create ? O_CREAT : 0);
    FileGuard file(::open(path.c_str(), flags, 0600));
    if (file.fd == -1) {
        if (!create && errno == ENOENT) return -1;
        throw std::system_error(errno, std::generic_category(), "Cannot open " + path.string());
    }
    struct stat info{};
    if (::fstat(file.fd, &info) == -1)
        throw std::system_error(errno, std::generic_category(), "Cannot inspect session file");
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        info.st_nlink != 1 || (info.st_mode & 077) != 0)
        throw std::runtime_error("Unsafe session file: " + path.string());
    return file.release();
}

bool tryLock(int fd) {
    int result;
    do { result = ::flock(fd, LOCK_EX | LOCK_NB); }
    while (result == -1 && errno == EINTR);
    if (result == 0) return true;
    if (errno == EWOULDBLOCK || errno == EAGAIN) return false;
    throw std::system_error(errno, std::generic_category(), "Cannot lock session file");
}

class StateWriteLock {
    FileGuard file;
public:
    explicit StateWriteLock(const std::filesystem::path& path)
        : file(openChecked(path.string() + ".update.lock", true)) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!tryLock(file.fd)) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Session update is busy; retry the command");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

void writeAt(int fd, const std::string& text, off_t offset) {
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = ::pwrite(fd, text.data() + written, text.size() - written,
                                      offset + static_cast<off_t>(written));
        if (count == -1 && errno == EINTR) continue;
        if (count == -1)
            throw std::system_error(errno, std::generic_category(), "Cannot write session state");
        if (count == 0) throw std::runtime_error("Session write made no progress");
        written += static_cast<std::size_t>(count);
    }
}

std::string readState(int fd) {
    std::string text;
    char buffer[512];
    while (true) {
        const ssize_t count = ::pread(fd, buffer, sizeof(buffer), static_cast<off_t>(text.size()));
        if (count == -1 && errno == EINTR) continue;
        if (count == -1)
            throw std::system_error(errno, std::generic_category(), "Cannot read session state");
        if (count == 0) break;
        text.append(buffer, static_cast<std::size_t>(count));
        if (text.size() > 4096) throw std::runtime_error("Session file is too large");
    }
    if (text.size() < 2 || (text[0] != '0' && text[0] != '1') || text[1] != '\n')
        throw std::runtime_error("Invalid session file format");
    return text;
}
}

std::filesystem::path BackgroundSession::defaultPath() { return "/var/run/can.session"; }

BackgroundSession::BackgroundSession(const std::string& serverName, bool tun,
    const std::filesystem::path& logPath, const std::filesystem::path& path,
    std::function<void()> readyCallback)
    : sessionPath(path), onReady(std::move(readyCallback)) {
    if (activeSession) throw std::runtime_error("Background session already registered");
    StateWriteLock updateLock(sessionPath);
    FileGuard file(openChecked(sessionPath, true));
    if (!tryLock(file.fd)) throw std::runtime_error("Another background Can session is already running");
    description = "PID: " + std::to_string(::getpid()) + "\nServer: " + serverName +
        "\nMode: " + (tun ? "TUN" : "SOCKS") + "\nLog: " + logPath.string() + "\n";
    if (::ftruncate(file.fd, 0) == -1)
        throw std::system_error(errno, std::generic_category(), "Cannot clear session state");
    writeAt(file.fd, "0\nState: starting\n" + description, 0);
    sessionFd = file.release();
    activeSession = this;
}

BackgroundSession::~BackgroundSession() {
    if (activeSession == this) activeSession = nullptr;
    if (sessionFd >= 0) ::close(sessionFd);
}

std::string BackgroundSession::status(const std::filesystem::path& path) {
    FileGuard file(openChecked(path, false));
    if (file.fd == -1) return "No active background session\n";
    StateWriteLock updateLock(path);
    if (tryLock(file.fd)) return "No active background session\n";
    const auto text = readState(file.fd);
    return std::string(text[0] == '1' ? "Stop requested\n" : "Background session active\n") +
        text.substr(2) + "Internet connectivity is not checked by status.\n";
}

bool BackgroundSession::hasStopRequest() noexcept {
    if (!activeSession) return false;
    if (activeSession->needToStop) return true;
    char flag = '\0';
    ssize_t count;
    do { count = ::pread(activeSession->sessionFd, &flag, 1, 0); }
    while (count == -1 && errno == EINTR);
    activeSession->needToStop = count != 1 || flag != '0';
    return activeSession->needToStop;
}

void BackgroundSession::markReady() {
    if (!activeSession || activeSession->ready) return;
    auto& session = *activeSession;
    {
        StateWriteLock updateLock(session.sessionPath);
        if (hasStopRequest()) throw std::runtime_error("Background startup was cancelled");
        const std::string state = "State: ready\n" + session.description;
        writeAt(session.sessionFd, state, 2);
        if (::ftruncate(session.sessionFd, static_cast<off_t>(state.size() + 2)) == -1)
            throw std::system_error(errno, std::generic_category(), "Cannot update session state");
        session.ready = true;
    }
    if (session.onReady) session.onReady();
}

void BackgroundSession::requestStop(const std::filesystem::path& path, std::chrono::milliseconds timeout) {
    FileGuard file(openChecked(path, false));
    if (file.fd == -1) throw std::runtime_error("No active background session");
    std::string identity;
    {
        StateWriteLock updateLock(path);
        if (tryLock(file.fd)) throw std::runtime_error("No active background session");
        identity = readState(file.fd);
        writeAt(file.fd, "1", 0);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            StateWriteLock updateLock(path);
            if (tryLock(file.fd)) return;
            if (readState(file.fd).substr(2) != identity.substr(2))
                throw std::runtime_error("Session changed while waiting for stop; inspect status");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("Stop requested, but session has not ended yet. Check its log and retry status.");
}
