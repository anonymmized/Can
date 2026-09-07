#include "CliOptions.hpp"
#include "LinkParser.hpp"
#include "NetworkTransaction.hpp"
#include "SystemCommand.hpp"
#include "XrayConfigBuilder.hpp"
#include "XrayProcess.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> void expectThrow(F action) {
    bool thrown = false;
    try { action(); } catch (const std::exception&) { thrown = true; }
    expect(thrown, "Expected an exception");
}

const std::string testLink =
    "vless://11111111-2222-3333-4444-555555555555@vpn.example.com:443?"
    "encryption=none&flow=xtls-rprx-vision&security=reality&sni=www.example.com&"
    "fp=firefox&pbk=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA&sid=0123456789abcdef&type=tcp#Test";

Server testServer() {
    return {"test", LinkParser().getParsedLink(testLink)};
}

struct EnvironmentGuard {
    const char* name;
    bool existed;
    std::string previous;
    explicit EnvironmentGuard(const char* name) : name(name), existed(std::getenv(name) != nullptr),
        previous(existed ? std::getenv(name) : "") { ::unsetenv(name); }
    ~EnvironmentGuard() { if (existed) ::setenv(name, previous.c_str(), 1); else ::unsetenv(name); }
};

int fakeXray(int argc, char** argv) {
    if (argc != 5 || std::string(argv[3]) != "-c") return 91;
    std::ifstream file(argv[4]);
    nlohmann::json config;
    file >> config;
    struct stat info{};
    if (::stat(argv[4], &info) != 0 || (info.st_mode & 0777) != 0600) return 92;
    struct sigaction action{};
    ::sigaction(SIGINT, nullptr, &action);
    if (action.sa_handler != SIG_DFL) return 93;
    if (config.contains("reportFd")) {
        const int fd = config["reportFd"].get<int>();
        const std::string path = argv[4];
        if (::write(fd, path.data(), path.size()) != static_cast<ssize_t>(path.size())) return 94;
        ::close(fd);
    }
    if (config.value("wait", false)) {
        while (true) ::pause();
    }
    return config.value("exitCode", 0);
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "run") return fakeXray(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--command-child") {
        std::cout << argv[2];
        return 17;
    }
    const auto self = std::filesystem::absolute(argv[0]).string();
    int failures = 0;
    auto test = [&](const char* name, const std::function<void()>& action) {
        try {
            action();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    test("VLESS fields", [] {
        const auto server = testServer();
        expect(server.linkData.host == "vpn.example.com", "host");
        expect(server.linkData.port == 443, "port");
        expect(server.linkData.fingerprint == "firefox", "fingerprint");
        expect(server.linkData.shortId == "0123456789abcdef", "short id");
        expect(server.linkData.flow == "xtls-rprx-vision", "flow");
    });
    test("Invalid VLESS URLs", [] {
        for (const std::string& url : {std::string("https://example.com"), std::string("vless://uuid@host:0"),
              std::string("vless://uuid@host:65536"), std::string("vless://uuid@host:443junk")}) {
            expectThrow([&] { LinkParser().getParsedLink(url); });
        }
    });
    test("SOCKS config and interface override", [] {
        XrayRuntimeOptions options;
        options.outboundInterface = "en7";
        options.socksPort = 19080;
        const auto config = XrayConfigBuilder().build(testServer(), options);
        expect(config["inbounds"][0]["protocol"] == "socks", "SOCKS inbound");
        expect(config["inbounds"][0]["port"] == 19080, "SOCKS port");
        expect(config["outbounds"][0]["streamSettings"]["sockopt"]["interface"] == "en7", "outbound interface");
        expect(config["outbounds"][0]["streamSettings"]["realitySettings"]["fingerprint"] == "firefox", "fingerprint preservation");
    });
    test("TUN config does not open SOCKS", [] {
        XrayRuntimeOptions options;
        options.outboundInterface = "en7";
        const auto config = XrayConfigBuilder().buildTun(testServer(), options, "utun123");
        expect(config["inbounds"].size() == 1, "only TUN inbound");
        const auto& inbound = config["inbounds"][0];
        expect(inbound["protocol"] == "tun", "TUN protocol");
        expect(!inbound.contains("port") && !inbound.contains("listen"), "TUN must not bind a SOCKS port");
        expect(inbound["settings"]["name"] == "utun123", "TUN name");
        expect(inbound["settings"]["MTU"] == 1500, "MTU");
        expect(!inbound["settings"].contains("autoSystemRoutingTable"), "26.3.27 compatibility");
        expect(config["outbounds"][0]["settings"]["vnext"][0]["users"][0]["flow"] ==
               "xtls-rprx-vision-udp443", "UDP/443 must not be silently rejected in TUN mode");
    });
    test("TUN rejects missing interface", [] {
        expectThrow([] { XrayConfigBuilder().buildTun(testServer(), {}, "utun123"); });
        XrayRuntimeOptions options;
        options.outboundInterface = "en7";
        expectThrow([&] { XrayConfigBuilder().buildTun(testServer(), options, ""); });
    });
    test("Builder rejects unsupported transport and security", [] {
        auto server = testServer();
        server.linkData.transport = "ws";
        expectThrow([&] { XrayConfigBuilder().build(server, {}); });
        server = testServer();
        server.linkData.security = "tls";
        expectThrow([&] { XrayConfigBuilder().build(server, {}); });
    });
    test("Strict server numbers", [] {
        expect(parseServerNumber("42") == 42, "valid ID");
        for (const char* text : {"0", "-1", "1abc", " 1", "+1", "9999999999999999999", ""}) {
            expectThrow([&] { parseServerNumber(text); });
        }
    });
    test("Connection flags and environment", [] {
        EnvironmentGuard guard("CAN_OUTBOUND_INTERFACE");
        ::setenv("CAN_OUTBOUND_INTERFACE", "en0", 1);
        expect(parseConnectionOptions({}).runtime.outboundInterface == "en0", "environment");
        auto options = parseConnectionOptions({"--interface", "en7", "--port", "19080", "--xray", "/tmp/xray-test"});
        expect(options.runtime.outboundInterface == "en7", "explicit override");
        expect(options.runtime.socksPort == 19080, "port override");
        expect(options.executable == "/tmp/xray-test", "executable override");
        options = parseConnectionOptions({"--tun", "--check"});
        expect(options.tun && options.check, "TUN preflight");
    });
    test("Invalid connection flags", [] {
        for (const std::vector<std::string>& arguments : {
                std::vector<std::string>{"--wat"}, {"--interface"}, {"--interface", "--tun"},
                {"--check"}, {"--port", "0"}, {"--port", "65536"}, {"--port", "12junk"},
                {"--tun", "--port", "1080"}}) {
            expectThrow([&] { parseConnectionOptions(arguments); });
        }
    });
    test("Network rollback reverses successful operations only", [] {
        std::vector<int> actions;
        NetworkTransaction transaction;
        transaction.apply("one", [&] { actions.push_back(1); }, [&] { actions.push_back(-1); });
        transaction.apply("two", [&] { actions.push_back(2); }, [&] { actions.push_back(-2); });
        expectThrow([&] {
            transaction.apply("conflict", [] { throw std::runtime_error("route exists"); },
                              [&] { actions.push_back(-3); });
        });
        transaction.rollback();
        transaction.rollback();
        expect(actions == std::vector<int>({1, 2, -2, -1}), "rollback order / existing route protection");
    });
    test("Network cleanup continues after an undo failure", [] {
        std::vector<int> undo;
        bool fail = true;
        NetworkTransaction transaction;
        transaction.apply("one", [] {}, [&] { undo.push_back(1); });
        transaction.apply("two", [] {}, [&] {
            if (fail) throw std::runtime_error("temporary error");
            undo.push_back(2);
        });
        expectThrow([&] { transaction.rollback(); });
        expect(undo == std::vector<int>({1}), "cleanup should continue");
        fail = false;
        transaction.rollback();
        expect(undo == std::vector<int>({1, 2}), "failed undo should remain retryable");
    });
    test("Network scope exit rolls back", [] {
        bool active = false;
        {
            NetworkTransaction transaction;
            transaction.apply("route", [&] { active = true; }, [&] { active = false; });
        }
        expect(!active, "destructor rollback");
    });
    test("System commands do not interpret shell syntax", [&] {
        const std::string literal = "one; $CAN_NOT_A_VARIABLE `not-a-command` two";
        const auto result = runSystemCommand({self, "--command-child", literal});
        expect(result.exitCode == 17, "child exit status");
        expect(result.output == literal, "arguments must be literal");
        expectThrow([&] { checkedSystemCommand({self, "--command-child", "error"}); });
        expectThrow([] { runSystemCommand({"relative-command"}); });
    });
    test("Executable lookup", [&] {
        expect(findExecutable(self) == self, "absolute executable");
        expectThrow([] { findExecutable("/nonexistent-can-test/xray"); });
    });
    test("Xray config has private permissions and is removed", [&] {
        int pipeFd[2];
        expect(::pipe(pipeFd) == 0, "report pipe");
        const int result = XrayProcess().run({{"reportFd", pipeFd[1]}, {"exitCode", 17}}, {}, self);
        ::close(pipeFd[1]);
        char buffer[4096];
        const auto count = ::read(pipeFd[0], buffer, sizeof(buffer));
        ::close(pipeFd[0]);
        expect(result == 17, "child configuration and exit code");
        expect(count > 0, "child reported config path");
        expect(!std::filesystem::exists(std::string(buffer, static_cast<std::size_t>(count))), "temporary config cleanup");
    });
    test("Xray startup callback failure stops child and cleans up", [&] {
        int cleaned = 0;
        XrayProcessHooks hooks;
        hooks.ready = [] { return true; };
        hooks.onReady = [] { throw std::runtime_error("route failed"); };
        hooks.onStop = [&] { ++cleaned; };
        expectThrow([&] { XrayProcess().run({{"wait", true}}, hooks, self); });
        expect(cleaned == 1, "cleanup called once");
    });
    test("SIGINT SIGTERM SIGHUP restore state and stop child", [&] {
        for (int signal : {SIGINT, SIGTERM, SIGHUP}) {
            int cleaned = 0;
            struct sigaction before{}, after{};
            ::sigaction(signal, nullptr, &before);
            XrayProcessHooks hooks;
            hooks.onReady = [signal] { ::raise(signal); };
            hooks.onStop = [&] { ++cleaned; };
            const int result = XrayProcess().run({{"wait", true}}, hooks, self);
            ::sigaction(signal, nullptr, &after);
            expect(result == 128 + signal, "signal exit code");
            expect(cleaned == 1, "signal cleanup");
            expect(before.sa_handler == after.sa_handler, "parent signal handler restored");
        }
    });
    test("Xray cleanup on normal child exit", [&] {
        int cleaned = 0;
        XrayProcessHooks hooks;
        hooks.onStop = [&] { ++cleaned; };
        expect(XrayProcess().run({{"exitCode", 0}}, hooks, self) == 0, "normal exit");
        expect(cleaned == 1, "normal cleanup");
        expectThrow([] { XrayProcess().run({}, {}, "/nonexistent-can-test/xray"); });
    });
    test("Xray exit before readiness does not change the network", [&] {
        int activated = 0, cleaned = 0;
        XrayProcessHooks hooks;
        hooks.ready = [] { return false; };
        hooks.onReady = [&] { ++activated; };
        hooks.onStop = [&] { ++cleaned; };
        expect(XrayProcess().run({{"exitCode", 17}}, hooks, self) == 17, "startup failure status");
        expect(activated == 0 && cleaned == 1, "no activation before readiness");
    });
    test("Server storage and delete renumbering", [] {
        struct TemporaryDirectory {
            std::filesystem::path previous = std::filesystem::current_path();
            std::filesystem::path directory;
            TemporaryDirectory() {
                auto pattern = (std::filesystem::temp_directory_path() / "can-tests-XXXXXX").string();
                char* created = ::mkdtemp(pattern.data());
                if (!created) throw std::runtime_error("mkdtemp");
                directory = created;
                std::filesystem::current_path(directory);
            }
            ~TemporaryDirectory() {
                std::error_code error;
                std::filesystem::current_path(previous, error);
                std::filesystem::remove_all(directory, error);
            }
        } temporary;
        ServerManager manager;
        expectThrow([&] { manager.addServer("../escape", testLink); });
        expectThrow([&] { manager.addServer("bad\nname", testLink); });
        manager.addServer("first", testLink);
        manager.addServer("second", testLink);
        expect(manager.getServer(1).serverName == "first", "first server");
        expect(manager.getServer(2).serverName == "second", "second server");
        expectThrow([&] { manager.addServer("second", testLink); });
        manager.deleteServer(1);
        expect(manager.getServer(1).serverName == "second", "renumbering");
        expectThrow([&] { manager.getServer(2); });
        manager.deleteServer(1);
        expectThrow([&] { manager.getServer(1); });
    });
    test("SOCKS rejects an occupied port", [] {
        const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
        expect(socket >= 0, "socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(socket);
            throw std::runtime_error("Test requires permission to bind a local loopback socket");
        }
        socklen_t length = sizeof(address);
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length);
        XrayRuntimeOptions options;
        options.socksPort = ntohs(address.sin_port);
        bool rejected = false;
        try { requireFreeSocksPort(options); } catch (const std::exception&) { rejected = true; }
        ::close(socket);
        expect(rejected, "occupied port rejection");
        requireFreeSocksPort(options);
    });
    std::cout << (failures == 0 ? "All tests passed\n" : "Tests failed\n");
    return failures == 0 ? 0 : 1;
}
