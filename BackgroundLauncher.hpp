#pragma once
#include <filesystem>
#include <functional>
#include <string>

class BackgroundLauncher {
public:
    using Ready = std::function<void()>;
    using Action = std::function<int(const std::filesystem::path&, const Ready&)>;
    static int run(const std::filesystem::path& logDirectory, const std::string& message,
                   const Action& action);
};
