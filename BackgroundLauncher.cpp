#include "BackgroundLauncher.hpp"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }
    void reset() { if (value >= 0) ::close(std::exchange(value, -1)); }
};

void check(int value, const char* message) {
    if (value == -1) throw std::system_error(errno, std::generic_category(), message);
}

void report(int fd, const std::string& message) {
    std::size_t sent = 0;
    while (sent < message.size()) {
#ifdef __APPLE__
        const int flags = 0;
#else
        const int flags = MSG_NOSIGNAL;
#endif
        const ssize_t count = ::send(fd, message.data() + sent, message.size() - sent, flags);
        if (count == -1 && errno == EINTR) continue;
        if (count == -1) throw std::system_error(errno, std::generic_category(), "Report startup");
        if (count == 0) throw std::runtime_error("Startup channel closed");
        sent += static_cast<std::size_t>(count);
    }
}

bool reap(pid_t pid, int& status) {
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid || (result == -1 && errno == ECHILD)) return true;
    if (result == -1 && errno != EINTR)
        throw std::system_error(errno, std::generic_category(), "Wait for background startup");
    return false;
}
}

int BackgroundLauncher::run(const std::filesystem::path& logDirectory, const std::string& message,
                            const Action& action) {
    std::string pattern = (logDirectory / "can-XXXXXX").string();
    Fd log(::mkstemp(pattern.data()));
    check(log.value, "Create log");
    check(::fcntl(log.value, F_SETFD, FD_CLOEXEC), "Configure log descriptor");
    int pair[2];
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "Create startup channel");
    Fd parent(pair[0]), child(pair[1]);
    for (int fd : {parent.value, child.value}) {
        check(::fcntl(fd, F_SETFD, FD_CLOEXEC), "Configure startup channel");
#ifdef __APPLE__
        int enabled = 1;
        check(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)), "Configure startup signals");
#endif
    }
    std::cout.flush();
    std::cerr.flush();
    const pid_t pid = ::fork();
    check(pid, "Create background process");
    if (pid == 0) {
        parent.reset();
        int exitCode = 1;
        bool announced = false;
        try {
            check(::setsid(), "Detach background process");
            check(::dup2(log.value, STDOUT_FILENO), "Redirect stdout");
            check(::dup2(log.value, STDERR_FILENO), "Redirect stderr");
            if (log.value > STDERR_FILENO) log.reset();
            Fd input(::open("/dev/null", O_RDONLY | O_CLOEXEC));
            check(input.value, "Open background stdin");
            check(::dup2(input.value, STDIN_FILENO), "Redirect stdin");
            if (input.value == STDIN_FILENO) input.value = -1;
            std::cout << std::unitbuf;
            std::cerr << std::unitbuf;
            const Ready ready = [&] {
                if (!announced) {
                    report(child.value, "READY\n");
                    announced = true;
                    child.reset();
                }
            };
            exitCode = action(pattern, ready);
            if (!announced)
                throw std::runtime_error("Connection ended before startup completed (exit " + std::to_string(exitCode) + ")");
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << '\n';
            if (!announced) {
                try { report(child.value, "ERROR: " + std::string(error.what()).substr(0, 2000) + "\n"); }
                catch (...) {}
            }
            exitCode = 1;
        } catch (...) {
            if (!announced) {
                try { report(child.value, "ERROR: Unknown startup error\n"); }
                catch (...) {}
            }
            exitCode = 1;
        }
        std::cout.flush();
        std::cerr.flush();
        ::_exit(exitCode);
    }

    child.reset();
    log.reset();
    int status = 0;
    bool exited = false;
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        std::string response;
        while (true) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Background startup timed out");
            pollfd descriptor{parent.value, POLLIN, 0};
            const int result = ::poll(&descriptor, 1, 100);
            if (result == -1 && errno == EINTR) continue;
            check(result, "Wait for startup response");
            if (result > 0) {
                char buffer[512];
                const ssize_t count = ::recv(parent.value, buffer, sizeof(buffer), 0);
                if (count == -1 && errno == EINTR) continue;
                check(static_cast<int>(count), "Read startup response");
                if (count == 0) throw std::runtime_error("Background process exited without confirming startup");
                response.append(buffer, static_cast<std::size_t>(count));
                if (response.size() > 4096) throw std::runtime_error("Invalid startup response");
                if (response.find('\n') != std::string::npos) {
                    if (response != "READY\n") throw std::runtime_error(response.substr(0, response.find('\n')));
                    exited = reap(pid, status);
                    if (exited) throw std::runtime_error("Background process exited immediately after startup");
                    std::cout << message << "\nPID: " << pid << "\nLog: " << pattern << '\n';
                    return 0;
                }
            }
            if (!exited) exited = reap(pid, status);
            if (exited && result == 0) throw std::runtime_error("Background process failed during startup");
        }
    } catch (const std::exception& error) {
        if (!exited) {
            ::kill(pid, SIGTERM);
            for (int attempt = 0; attempt < 100 && !exited; ++attempt) {
                try { exited = reap(pid, status); } catch (...) { break; }
                if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        throw std::runtime_error(std::string(error.what()) + "\nLog: " + pattern +
            (exited ? "" : "\nCancellation requested; inspect status before retrying."));
    }
}
