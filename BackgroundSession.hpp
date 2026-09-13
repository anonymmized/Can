#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

class BackgroundSession {
    inline static BackgroundSession* activeSession = nullptr;
    int sessionFd = -1;
    bool needToStop = false;
    bool ready = false;
    std::filesystem::path sessionPath;
    std::string description;
    std::function<void()> onReady;
public:
    static std::filesystem::path defaultPath();
    BackgroundSession(const std::string& serverName, bool tun,
                      const std::filesystem::path& logPath,
                      const std::filesystem::path& path = defaultPath(),
                      std::function<void()> readyCallback = {});
    BackgroundSession(const BackgroundSession&) = delete;
    BackgroundSession& operator=(const BackgroundSession&) = delete;
    ~BackgroundSession();
    static std::string status(const std::filesystem::path& path = defaultPath());
    static bool hasStopRequest() noexcept;
    static void markReady();
    static void requestStop(const std::filesystem::path& path = defaultPath(),
                            std::chrono::milliseconds timeout = std::chrono::seconds(15));
};
