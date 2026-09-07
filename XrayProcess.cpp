#include "XrayProcess.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <chrono>
#include <iostream>
#include <thread>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
    struct TemporaryConfig {
        std::string path;
        bool created = false;
        ~TemporaryConfig() {
            if (created) {
                ::unlink(path.c_str());
            }
        }
    };

    void checkSpawnResult(int result, const char* message) {
        if (result != 0) {
            throw std::system_error(result, std::generic_category(), message);
        }
    }

    struct SpawnAttributes {
        posix_spawnattr_t value;
        SpawnAttributes() {
            checkSpawnResult(posix_spawnattr_init(&value), "Couldn't initialize process attributes");
        }
        ~SpawnAttributes() {
            posix_spawnattr_destroy(&value);
        }
    };

    volatile sig_atomic_t pendingSignal = 0;
    void requestStop(int signal) { pendingSignal = signal; }

    class ParentSignalGuard {
        struct sigaction previousActions[3] {};
        const int signals[3] = {SIGINT, SIGTERM, SIGHUP};
        int installed = 0;
    public:
        ParentSignalGuard() {
            pendingSignal = 0;
            for (int signal : signals) {
                struct sigaction action {};
                action.sa_handler = requestStop;
                sigemptyset(&action.sa_mask);
                if (::sigaction(signal, &action, &previousActions[installed]) == -1) {
                    const int error = errno;
                    restore();
                    throw std::system_error(error, std::generic_category(), "Couldn't configure signals");
                }
                ++installed;
            }
        }
        void restore() noexcept {
            while (installed > 0) {
                --installed;
                ::sigaction(signals[installed], &previousActions[installed], nullptr);
            }
        }
        ~ParentSignalGuard() { restore(); }
    };

    struct ChildProcess {
        pid_t pid;
        bool reaped = false;
        int status = 0;

        bool poll() {
            if (reaped) return true;
            const pid_t result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid) reaped = true;
            else if (result == -1 && errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "Couldn't wait for xray");
            }
            return reaped;
        }

        void stop(int signal) noexcept {
            if (reaped) return;
            ::kill(pid, signal);
            for (int attempt = 0; attempt < 100; ++attempt) {
                const pid_t result = ::waitpid(pid, &status, WNOHANG);
                if (result == pid || (result == -1 && errno == ECHILD)) {
                    reaped = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
            reaped = true;
        }
        ~ChildProcess() { stop(SIGTERM); }
    };
}

bool XrayProcess::stopRequested() { return pendingSignal != 0; }

int XrayProcess::run(const nlohmann::json& config, const XrayProcessHooks& hooks,
                     const std::string& executable) const {
    const std::string content = config.dump(4);
    TemporaryConfig configFile;
    configFile.path = (std::filesystem::temp_directory_path() / "can-xray-XXXXXX").string();
    const int fd = ::mkstemp(configFile.path.data());
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "Couldn't create temporary Xray config");
    }
    configFile.created = true;
    using FileHandle = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    FileHandle file(::fdopen(fd, "w"), &std::fclose);
    if (!file) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "Couldn't open config stream");
    }
    if (std::fwrite(content.data(), 1, content.size(), file.get()) != content.size()) {
        throw std::runtime_error("Couldn't write Xray config");
    }
    if (std::fclose(file.release()) != 0) {
        throw std::system_error(errno, std::generic_category(), "Couldn't finish writing Xray config");
    }

    SpawnAttributes attributes;
    sigset_t defaultSignals;
    sigemptyset(&defaultSignals);
    for (int signal : {SIGINT, SIGTERM, SIGHUP}) sigaddset(&defaultSignals, signal);
    checkSpawnResult(posix_spawnattr_setsigdefault(&attributes.value, &defaultSignals), "Couldn't configure child signals");
    sigset_t emptyMask;
    sigemptyset(&emptyMask);
    checkSpawnResult(posix_spawnattr_setsigmask(&attributes.value, &emptyMask), "Couldn't reset child signal mask");
    // A separate process group lets the parent restore the network BEFORE stopping Xray.
    checkSpawnResult(posix_spawnattr_setpgroup(&attributes.value, 0), "Couldn't configure process group");
    checkSpawnResult(posix_spawnattr_setflags(&attributes.value,
        POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP),
        "Couldn't configure process flags");
    ParentSignalGuard signalGuard;
    char command[] = "run";
    char format[] = "-format=json";
    char configOption[] = "-c";
    char* arguments[] = { const_cast<char*>(executable.c_str()), command, format,
                         configOption, configFile.path.data(), nullptr };
    pid_t childPid;
    checkSpawnResult(::posix_spawnp(&childPid, executable.c_str(), nullptr, &attributes.value,
                                   arguments, environ), "Couldn't launch xray");
    ChildProcess child{childPid};
    bool cleanupDone = false;
    auto cleanup = [&] {
        if (!cleanupDone) {
            cleanupDone = true;
            if (hooks.onStop) hooks.onStop();
        }
    };
    try {
        bool activated = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!child.poll() && !stopRequested()) {
            if (!activated) {
                if (!hooks.ready || hooks.ready()) {
                    if (hooks.onReady) hooks.onReady();
                    activated = true;
                } else if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("Xray did not create a usable TUN interface within 10 seconds");
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        cleanup();
        if (stopRequested()) {
            const int signal = pendingSignal;
            child.stop(signal);
            return 128 + signal;
        }
    } catch (...) {
        try { cleanup(); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
        child.stop(SIGTERM);
        throw;
    }
    const int status = child.status;
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}
