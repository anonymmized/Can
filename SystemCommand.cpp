#include "SystemCommand.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

extern char** environ;

namespace {
struct FileDescriptor {
    int value = -1;
    ~FileDescriptor() { if (value >= 0) ::close(value); }
};
void check(int result) {
    if (result != 0) throw std::system_error(result, std::generic_category(), "System command");
}
struct Actions {
    posix_spawn_file_actions_t value;
    Actions() { check(posix_spawn_file_actions_init(&value)); }
    ~Actions() { posix_spawn_file_actions_destroy(&value); }
};
struct Attributes {
    posix_spawnattr_t value;
    Attributes() { check(posix_spawnattr_init(&value)); }
    ~Attributes() { posix_spawnattr_destroy(&value); }
};
}

CommandResult runSystemCommand(const std::vector<std::string>& arguments) {
    if (arguments.empty() || arguments[0].empty() || arguments[0][0] != '/') {
        throw std::invalid_argument("System command needs an absolute executable path");
    }
    int pipeFd[2];
    if (::pipe(pipeFd) == -1) throw std::system_error(errno, std::generic_category(), "pipe");
    FileDescriptor reader{pipeFd[0]}, writer{pipeFd[1]};
    if (::fcntl(reader.value, F_SETFD, FD_CLOEXEC) == -1 ||
        ::fcntl(writer.value, F_SETFD, FD_CLOEXEC) == -1 ||
        ::fcntl(reader.value, F_SETFL, O_NONBLOCK) == -1) {
        throw std::system_error(errno, std::generic_category(), "fcntl");
    }
    Actions actions;
    check(posix_spawn_file_actions_adddup2(&actions.value, writer.value, STDOUT_FILENO));
    check(posix_spawn_file_actions_adddup2(&actions.value, writer.value, STDERR_FILENO));
    check(posix_spawn_file_actions_addclose(&actions.value, reader.value));
    check(posix_spawn_file_actions_addclose(&actions.value, writer.value));
    Attributes attributes;
    sigset_t defaults, mask;
    sigemptyset(&defaults);
    for (int signal : {SIGINT, SIGTERM, SIGHUP}) sigaddset(&defaults, signal);
    sigemptyset(&mask);
    check(posix_spawnattr_setsigdefault(&attributes.value, &defaults));
    check(posix_spawnattr_setsigmask(&attributes.value, &mask));
    check(posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t child;
    check(posix_spawn(&child, argv[0], &actions.value, &attributes.value, argv.data(), environ));
    ::close(writer.value);
    writer.value = -1;
    bool reaped = false;
    int status = 0;
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    try {
        while (true) {
            char buffer[4096];
            ssize_t count;
            while ((count = ::read(reader.value, buffer, sizeof(buffer))) > 0) {
                output.append(buffer, static_cast<std::size_t>(count));
                if (output.size() > 1024 * 1024) throw std::runtime_error("System command output too large");
            }
            if (count < 0 && errno != EAGAIN && errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "read command output");
            }
            if (!reaped) {
                const pid_t result = ::waitpid(child, &status, WNOHANG);
                if (result == child) reaped = true;
                else if (result == -1 && errno != EINTR) {
                    throw std::system_error(errno, std::generic_category(), "waitpid");
                }
            }
            if (reaped && count == 0) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("System command timed out: " + arguments[0]);
            }
            pollfd pollFd{reader.value, POLLIN, 0};
            ::poll(&pollFd, 1, 25);
        }
    } catch (...) {
        if (!reaped) {
            ::kill(child, SIGKILL);
            while (::waitpid(child, &status, 0) == -1 && errno == EINTR) {}
        }
        throw;
    }
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), std::move(output)};
}

std::string checkedSystemCommand(const std::vector<std::string>& arguments) {
    auto result = runSystemCommand(arguments);
    if (result.exitCode != 0) {
        throw std::runtime_error(arguments.front() + " failed: " + result.output);
    }
    return std::move(result.output);
}

std::string findExecutable(const std::string& name) {
    auto usable = [](const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && ::access(path.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string::npos) {
        const auto path = std::filesystem::absolute(name);
        if (usable(path)) return path.string();
    } else {
        if (const char* path = std::getenv("PATH")) {
            std::istringstream paths(path);
            std::string directory;
            while (std::getline(paths, directory, ':')) {
                if (directory.empty()) continue;
                const auto candidate = std::filesystem::path(directory) / name;
                if (usable(candidate)) return std::filesystem::absolute(candidate).string();
            }
        }
        for (const char* directory : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"}) {
            const auto candidate = std::filesystem::path(directory) / name;
            if (usable(candidate)) return candidate.string();
        }
    }
    throw std::runtime_error("Executable not found: " + name + ". Install Xray or use --xray /absolute/path/to/xray");
}
