#include "ArgumentHandler.hpp"
#include "BackgroundLauncher.hpp"
#include "BackgroundSession.hpp"
#include "XrayProcess.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

namespace {
void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Action> void expectError(Action action, const std::string& fragment) {
    try { action(); }
    catch (const std::exception& error) {
        expect(std::string(error.what()).find(fragment) != std::string::npos,
               "Unexpected error: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Expected error: " + fragment);
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "can-background-tests-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("mkdtemp");
        path = created;
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    auto session() const { return path / "session"; }
};

pid_t sessionPid(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line))
        if (line.rfind("PID: ", 0) == 0) return static_cast<pid_t>(std::stoi(line.substr(5)));
    throw std::runtime_error("Missing session PID");
}

struct ChildGuard {
    pid_t pid;
    ~ChildGuard() {
        if (pid <= 0) return;
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == 0) {
            ::kill(pid, SIGTERM);
            while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
        }
    }
    void wait() {
        for (int attempt = 0; attempt < 200; ++attempt) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("Background child did not exit");
    }
};

int command(std::vector<std::string> arguments) {
    std::vector<char*> pointers;
    for (auto& argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return ArgumentHandler(static_cast<int>(arguments.size()), pointers.data()).run();
}
}

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "run") {
        std::ifstream file(argv[4]);
        nlohmann::json config;
        file >> config;
        std::ofstream(config.at("marker").get<std::string>()) << "ready";
        ::alarm(15);
        while (true) ::pause();
    }
    const auto self = std::filesystem::absolute(argv[0]).string();
    int failures = 0;
    auto test = [&](const char* name, const std::function<void()>& action) {
        try { action(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    test("Missing session is inactive, not created by status or stop", [] {
        Directory directory;
        expect(BackgroundSession::status(directory.session()) == "No active background session\n", "status");
        expectError([&] { BackgroundSession::requestStop(directory.session()); }, "No active");
        expect(!std::filesystem::exists(directory.session()), "no new state file");
    });

    test("Separate locks allow startup, status, ready and clean release", [] {
        Directory directory;
        int notifications = 0;
        {
            BackgroundSession session("test", true, directory.path / "log", directory.session(), [&] { ++notifications; });
            expect(BackgroundSession::status(directory.session()).find("State: starting") != std::string::npos, "starting");
            expect(!BackgroundSession::hasStopRequest(), "not stopped");
            BackgroundSession::markReady();
            BackgroundSession::markReady();
            expect(notifications == 1, "exactly one readiness notification");
            const auto state = BackgroundSession::status(directory.session());
            expect(state.find("Mode: TUN") != std::string::npos && state.find("State: ready") != std::string::npos, "ready TUN");
            struct stat info{};
            expect(::stat(directory.session().c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "private state");
        }
        expect(BackgroundSession::status(directory.session()) == "No active background session\n", "released");
        expectError([&] { BackgroundSession::requestStop(directory.session()); }, "No active");
    });

    test("Stale legacy state does not prevent restart", [] {
        Directory directory;
        std::ofstream(directory.session()) << "1";
        ::chmod(directory.session().c_str(), 0600);
        expect(BackgroundSession::status(directory.session()) == "No active background session\n", "legacy inactive");
        BackgroundSession session("new", false, directory.path / "log", directory.session());
        expect(!BackgroundSession::hasStopRequest(), "old stop reset");
        expect(BackgroundSession::status(directory.session()).find("Server: new") != std::string::npos, "new owner");
    });

    test("Readiness cannot overwrite a pending stop", [] {
        Directory directory;
        BackgroundSession session("test", false, directory.path / "log", directory.session());
        expectError([&] { BackgroundSession::requestStop(directory.session(), 0ms); }, "has not ended");
        expect(BackgroundSession::hasStopRequest(), "stop flag");
        expectError([] { BackgroundSession::markReady(); }, "cancelled");
        expect(BackgroundSession::status(directory.session()).find("Stop requested") == 0, "stop preserved");
    });

    test("Unsafe permissions and symlinks are rejected", [] {
        Directory directory;
        const auto target = directory.path / "target";
        std::ofstream(target) << "untouched";
        ::chmod(target.c_str(), 0644);
        expectError([&] { BackgroundSession session("test", false, "log", target); }, "Unsafe session file");
        std::filesystem::create_symlink(target, directory.session());
        expectError([&] { BackgroundSession::status(directory.session()); }, "Cannot open");
        std::ifstream file(target);
        std::string contents;
        file >> contents;
        expect(contents == "untouched", "target preserved");
    });

    test("Background startup failures are returned to the terminal", [] {
        Directory directory;
        expectError([&] {
            BackgroundLauncher::run(directory.path, "must not succeed", [&](const auto& log, const auto& ready) -> int {
                BackgroundSession session("test", false, log, directory.session(), ready);
                throw std::runtime_error("deliberate startup failure");
            });
        }, "deliberate startup failure");
        expect(BackgroundSession::status(directory.session()) == "No active background session\n", "failure released lock");
    });

    test("Exit without readiness is a startup failure", [] {
        Directory directory;
        expectError([&] {
            BackgroundLauncher::run(directory.path, "must not succeed", [](const auto&, const auto&) { return 17; });
        }, "before startup completed");
    });

    test("Background Xray lifecycle, duplicate protection and cooperative stop", [&] {
        Directory directory;
        const auto marker = directory.path / "fake-xray-ready";
        const auto cleanup = directory.path / "cleanup";
        BackgroundLauncher::run(directory.path, "Test background ready", [&](const auto& log, const auto& ready) {
            ::alarm(15);
            BackgroundSession session("test", false, log, directory.session(), ready);
            XrayProcessHooks hooks;
            hooks.ready = [&] { return std::filesystem::exists(marker); };
            hooks.onStop = [&] { std::ofstream(cleanup) << "cleaned"; };
            return XrayProcess().run({{"marker", marker.string()}}, hooks, self);
        });
        ChildGuard child{sessionPid(directory.session())};
        expect(std::filesystem::exists(marker), "exec completed before readiness");
        expect(BackgroundSession::status(directory.session()).find("State: ready") != std::string::npos, "ready owner");
        expectError([&] {
            BackgroundLauncher::run(directory.path, "must not succeed", [&](const auto& log, const auto& ready) {
                BackgroundSession session("duplicate", false, log, directory.session(), ready);
                return 0;
            });
        }, "Another background Can session");
        expect(BackgroundSession::status(directory.session()).find("Server: test") != std::string::npos, "original unchanged");
        BackgroundSession::requestStop(directory.session(), 5s);
        child.wait();
        expect(std::filesystem::exists(cleanup), "cleanup executed before release");
        expect(BackgroundSession::status(directory.session()) == "No active background session\n", "inactive after stop");
        expectError([&] { BackgroundSession::requestStop(directory.session()); }, "No active");
    });

    test("CLI show/delete dispatch and argument checks", [] {
        Directory directory;
        struct CwdGuard {
            std::filesystem::path previous = std::filesystem::current_path();
            ~CwdGuard() { std::filesystem::current_path(previous); }
        } cwd;
        std::filesystem::current_path(directory.path);
        expectError([] { command({"can", "show"}); }, "Usage: can show");
        expectError([] { command({"can", "delete"}); }, "Usage: can delete");
        expect(command({"can", "add", "test", "vless://11111111-2222-3333-4444-555555555555@example.com:443?encryption=none&type=tcp"}) == 0, "add");
        expect(command({"can", "show", "1"}) == 0, "show");
        expect(command({"can", "delete", "1"}) == 0, "delete");
        expectError([] { command({"can", "show", "1"}); }, "Invalid server number");
    });
    return failures == 0 ? 0 : 1;
}
