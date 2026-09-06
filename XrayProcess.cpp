#include "XrayProcess.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

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

    class ParentSigintGuard {
        private:
            struct sigaction previousAction {};
        public:
            ParentSigintGuard() {
                struct sigaction action {};
                action.sa_handler = SIG_IGN;
                sigemptyset(&action.sa_mask);
                if (::sigaction(SIGINT, &action, &previousAction) == -1) {
                    throw std::system_error(errno, std::generic_category(), "Couldn't configure SIGINT");
                }
            }
            ~ParentSigintGuard() {
                ::sigaction(SIGINT, &previousAction, nullptr);
            }
    };
}

int XrayProcess::run(const nlohmann::json& config) const {
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
    sigaddset(&defaultSignals, SIGINT);
    checkSpawnResult(posix_spawnattr_setsigdefault(&attributes.value, &defaultSignals), "Couldn't configure child signals");
    checkSpawnResult(posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETSIGDEF), "Couldn't configure process flags");
    ParentSigintGuard signalGuard;
    char executable[] = "xray";
    char command[] = "run";
    char format[] = "-format=json";
    char configOption[] = "-c";
    char* arguments[] = { executable, command, format, configOption, configFile.path.data(), nullptr };
    pid_t childPid;
    checkSpawnResult(::posix_spawnp(&childPid, executable, nullptr, &attributes.value, arguments, environ), "Couldn't launch xray");
    int status = 0;
    pid_t result;
    do {
        result = ::waitpid(childPid, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        throw std::system_error(errno, std::generic_category(), "Couldn't wait for xray");
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}
