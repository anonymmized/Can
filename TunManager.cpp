#include "TunManager.hpp"

#include "NetworkTransaction.hpp"
#include "SystemCommand.hpp"
#include "XrayProcess.hpp"

#include <iostream>
#include <stdexcept>

#ifdef __APPLE__
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <memory>
#include <net/if.h>
#include <netdb.h>
#include <sstream>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace {
template<class T> class CfHandle {
    T value;
public:
    explicit CfHandle(T value) : value(value) {
        if (!value) throw std::runtime_error("macOS SystemConfiguration allocation failed");
    }
    ~CfHandle() { CFRelease(value); }
    CfHandle(const CfHandle&) = delete;
    CfHandle& operator=(const CfHandle&) = delete;
    T get() const { return value; }
};

bool isTunnel(const std::string& name) {
    return name.rfind("utun", 0) == 0 || name.rfind("tun", 0) == 0 ||
           name.rfind("tap", 0) == 0 || name.rfind("ppp", 0) == 0 ||
           name.rfind("ipsec", 0) == 0 || name.rfind("gif", 0) == 0;
}

std::string routeField(const std::string& output, const std::string& field) {
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string key, value;
        if (words >> key >> value && key == field + ":") return value;
    }
    return "";
}

std::string physicalInterface() {
    const auto route = checkedSystemCommand({"/sbin/route", "-n", "get", "default"});
    const auto name = routeField(route, "interface");
    if (name.empty() || name == "lo0") {
        throw std::runtime_error("No physical IPv4 default route was found");
    }
    if (isTunnel(name)) {
        throw std::runtime_error("Another VPN owns the default route (" + name +
            "). Disconnect HAPP/other VPN before --tun. No network settings were changed.");
    }
    for (const char* target : {"1.1.1.1", "129.1.1.1"}) {
        const auto actual = checkedSystemCommand({"/sbin/route", "-n", "get", target});
        const auto other = routeField(actual, "interface");
        if (other != name) {
            throw std::runtime_error("Conflicting route to " + std::string(target) +
                " via " + other + ". No network settings were changed.");
        }
    }
    return name;
}

std::string physicalGateway() {
    const auto route = checkedSystemCommand({"/sbin/route", "-n", "get", "default"});
    const auto gateway = routeField(route, "gateway");
    if (gateway.empty() || gateway == "-" || gateway == "0.0.0.0") {
        throw std::runtime_error("No physical IPv4 gateway was found");
    }
    return gateway;
}

std::string ipv4Address(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int error = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (error != 0) throw std::runtime_error("Cannot resolve VPN server: " + std::string(gai_strerror(error)));
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owner(result, &freeaddrinfo);
    char address[INET_ADDRSTRLEN];
    if (!result || !::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr,
                                address, sizeof(address))) {
        throw std::runtime_error("VPN server has no usable IPv4 address");
    }
    return address;
}

class SessionLock {
    int fd = -1;
public:
    SessionLock() {
        fd = ::open("/var/run/can-tun.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd == -1) throw std::system_error(errno, std::generic_category(), "TUN lock");
        struct stat info{};
        if (::fstat(fd, &info) == -1 || !S_ISREG(info.st_mode) || info.st_uid != 0 || info.st_nlink != 1) {
            ::close(fd);
            throw std::runtime_error("Unsafe TUN lock file");
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) == -1) {
            ::close(fd);
            throw std::runtime_error("Another Can TUN session is already running");
        }
    }
    ~SessionLock() { ::close(fd); }
};

class TemporaryDns {
    SCDynamicStoreRef store = nullptr;
public:
    ~TemporaryDns() { reset(); }
    void reset() noexcept {
        if (store) CFRelease(store);
        store = nullptr;
    }
    void start(const std::string& tunName) {
        const void* optionKeys[] = {kSCDynamicStoreUseSessionKeys};
        const void* optionValues[] = {kCFBooleanTrue};
        CfHandle<CFDictionaryRef> options(CFDictionaryCreate(nullptr, optionKeys, optionValues, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        store = SCDynamicStoreCreateWithOptions(nullptr, CFSTR("Can TUN DNS"), options.get(), nullptr, nullptr);
        if (!store) throw std::runtime_error("Cannot open temporary macOS DNS session");

        const void* addresses[] = {CFSTR("1.1.1.1"), CFSTR("9.9.9.9")};
        const void* domains[] = {CFSTR("")};
        CfHandle<CFArrayRef> servers(CFArrayCreate(nullptr, addresses, 2, &kCFTypeArrayCallBacks));
        CfHandle<CFArrayRef> matches(CFArrayCreate(nullptr, domains, 1, &kCFTypeArrayCallBacks));
        int priority = 1;
        CfHandle<CFNumberRef> order(CFNumberCreate(nullptr, kCFNumberIntType, &priority));
        const void* keys[] = {kSCPropNetDNSServerAddresses, kSCPropNetDNSSupplementalMatchDomains,
                             kSCPropNetDNSSearchOrder};
        const void* values[] = {servers.get(), matches.get(), order.get()};
        CfHandle<CFDictionaryRef> dns(CFDictionaryCreate(nullptr, keys, values, 3,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        const auto keyName = "State:/Network/Service/Can-" + std::to_string(::getpid()) + "-" + tunName + "/DNS";
        CfHandle<CFStringRef> key(CFStringCreateWithCString(nullptr, keyName.c_str(), kCFStringEncodingUTF8));
        if (!SCDynamicStoreAddValue(store, key.get(), dns.get())) {
            throw std::runtime_error("Cannot install temporary DNS: " + std::string(SCErrorString(SCError())));
        }
    }
};

bool tunIsReady(const std::string& name) {
    ifaddrs* addresses = nullptr;
    if (::getifaddrs(&addresses) == -1) {
        throw std::system_error(errno, std::generic_category(), "getifaddrs");
    }
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> owner(addresses, &freeifaddrs);
    for (const ifaddrs* item = addresses; item; item = item->ifa_next) {
        if (item->ifa_addr && name == item->ifa_name &&
            item->ifa_addr->sa_family == AF_INET && (item->ifa_flags & IFF_UP)) return true;
    }
    return false;
}

class TunSession {
    TunPlan plan;
    std::uint16_t healthPort;
    NetworkTransaction routes;
    TemporaryDns dns;

    void addRoute(const std::string& family, const std::string& destination, bool host = false) {
        const std::string kind = host ? "-host" : "-net";
        const std::vector<std::string> add{"/sbin/route", "-n", "add", family, kind,
                                           destination, "-interface", plan.tunInterface};
        const std::vector<std::string> remove{"/sbin/route", "-n", "delete", family, kind,
                                              destination, "-interface", plan.tunInterface};
        routes.apply(destination, [&] {
            if (XrayProcess::stopRequested()) throw std::runtime_error("TUN startup interrupted");
            checkedSystemCommand(add);
        }, [name = plan.tunInterface, remove] {
            if (::if_nametoindex(name.c_str()) != 0) checkedSystemCommand(remove);
        });
    }

    void addServerRoute() {
        const std::vector<std::string> add{"/sbin/route", "-n", "add", "-inet", "-host",
                                           plan.serverAddress, plan.serverGateway};
        const std::vector<std::string> remove{"/sbin/route", "-n", "delete", "-inet", "-host",
                                              plan.serverAddress, plan.serverGateway};
        routes.apply("vpn-server", [&] {
            if (XrayProcess::stopRequested()) throw std::runtime_error("TUN startup interrupted");
            checkedSystemCommand(add);
        }, [remove] {
            checkedSystemCommand(remove);
        });
    }

public:
    TunSession(TunPlan plan, std::uint16_t healthPort) : plan(std::move(plan)), healthPort(healthPort) {}
    bool ready() const { return tunIsReady(plan.tunInterface); }
    void activate() {
        if (physicalInterface() != plan.outboundInterface) {
            throw std::runtime_error("Default interface changed during startup; retry the connection");
        }
        if (XrayProcess::stopRequested()) throw std::runtime_error("TUN startup interrupted");
        const auto response = checkedSystemCommand({"/usr/bin/curl", "--fail", "--silent", "--show-error",
            "--connect-timeout", "3", "--max-time", "8", "--retry", "2", "--retry-connrefused",
            "--retry-delay", "1", "--retry-max-time", "8", "--noproxy", "", "--proxy",
            "socks5h://127.0.0.1:" + std::to_string(healthPort), "https://api.ipify.org"});
        in_addr exitAddress{};
        if (::inet_pton(AF_INET, response.c_str(), &exitAddress) != 1) {
            throw std::runtime_error("VPN connectivity check returned an unexpected response; routes were not changed");
        }
        if (physicalInterface() != plan.outboundInterface || XrayProcess::stopRequested()) {
            throw std::runtime_error("Network changed or startup was interrupted during the VPN connectivity check");
        }
        checkedSystemCommand({"/sbin/ifconfig", plan.tunInterface, "inet6", "fd73:616e::2",
                              "prefixlen", "64", "alias"});
        addServerRoute();
        addRoute("-inet", "0.0.0.0/1");
        addRoute("-inet", "128.0.0.0/1");
        addRoute("-inet6", "::/1");
        addRoute("-inet6", "8000::/1");
        addRoute("-inet", "1.1.1.1", true);
        addRoute("-inet", "9.9.9.9", true);
        if (XrayProcess::stopRequested()) throw std::runtime_error("TUN startup interrupted");
        dns.start(plan.tunInterface);
        std::cout << "TUN active: " << plan.tunInterface << " -> " << plan.outboundInterface
                  << ". IPv4/IPv6 and DNS routed through Xray. Ctrl+C restores the network.\n";
    }
    void stop() {
        dns.reset();
        routes.rollback();
    }
};

std::uint16_t unusedLoopbackPort() {
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == -1) throw std::system_error(errno, std::generic_category(), "Health-check socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        const int error = errno;
        ::close(socket);
        throw std::system_error(error, std::generic_category(), "Health-check port");
    }
    socklen_t length = sizeof(address);
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == -1) {
        const int error = errno;
        ::close(socket);
        throw std::system_error(error, std::generic_category(), "Health-check port address");
    }
    ::close(socket);
    return ntohs(address.sin_port);
}
}

TunPlan TunManager::inspect(const Server& server, const std::string& requestedInterface) const {
    const auto physical = physicalInterface();
    if (!requestedInterface.empty() && requestedInterface != physical) {
        throw std::runtime_error("Requested interface differs from the active physical default (" +
                                 physical + "). Refusing to guess TUN routing.");
    }
    std::string tun;
    for (int index = 100; index < 1000; ++index) {
        const auto candidate = "utun" + std::to_string(index);
        if (::if_nametoindex(candidate.c_str()) == 0) { tun = candidate; break; }
    }
    if (tun.empty()) throw std::runtime_error("No unused utun interface name found");
    return {tun, physical, ipv4Address(server.linkData.host), physicalGateway()};
}

std::string TunManager::socksOutboundInterface() const {
    const auto route = checkedSystemCommand({"/sbin/route", "-n", "get", "default"});
    if (!isTunnel(routeField(route, "interface"))) return "";
    const auto table = checkedSystemCommand({"/usr/sbin/netstat", "-rn", "-f", "inet"});
    std::set<std::string> candidates;
    std::istringstream lines(table);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string destination, gateway, flags, interface;
        in_addr address{};
        if (fields >> destination >> gateway >> flags >> interface && destination == "default" &&
            !isTunnel(interface) && interface != "lo0" &&
            ::inet_pton(AF_INET, gateway.c_str(), &address) == 1) {
            candidates.insert(interface);
        }
    }
    if (candidates.size() != 1) {
        throw std::runtime_error("Another VPN is active and the physical interface is ambiguous. Use --interface <name>.");
    }
    return *candidates.begin();
}

int TunManager::run(const Server& server, XrayRuntimeOptions options, const std::string& executable) const {
    if (::geteuid() != 0) {
        throw std::runtime_error("TUN requires administrator rights: sudo ./build/can connect <id> --tun. "
                                 "Use --tun --check for a read-only preflight.");
    }
    auto plan = inspect(server, options.outboundInterface);
    SessionLock lock;
    Server resolvedServer = server;
    resolvedServer.linkData.host = plan.serverAddress;
    options.outboundInterface = plan.outboundInterface;
    options.socksPort = unusedLoopbackPort();
    auto config = XrayConfigBuilder().buildTun(resolvedServer, options, plan.tunInterface);
    config["inbounds"].push_back(XrayConfigBuilder().build(resolvedServer, options)["inbounds"][0]);
    config["inbounds"][1]["tag"] = "health-check";
    TunSession session(plan, options.socksPort);
    XrayProcessHooks hooks;
    hooks.ready = [&] { return session.ready(); };
    hooks.onReady = [&] { session.activate(); };
    hooks.onStop = [&] { session.stop(); };
    return XrayProcess().run(config, hooks, executable);
}

#else
std::string TunManager::socksOutboundInterface() const { return ""; }

TunPlan TunManager::inspect(const Server&, const std::string&) const {
    throw std::runtime_error("System TUN is currently implemented for macOS only; SOCKS mode remains available");
}
int TunManager::run(const Server&, XrayRuntimeOptions, const std::string&) const {
    throw std::runtime_error("System TUN is currently implemented for macOS only; SOCKS mode remains available");
}
#endif
