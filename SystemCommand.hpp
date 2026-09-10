#pragma once

#include <string>
#include <vector>

struct CommandResult {
    int exitCode;
    std::string output;
};

CommandResult runSystemCommand(const std::vector<std::string>& arguments);
std::string checkedSystemCommand(const std::vector<std::string>& arguments);
std::string findExecutable(const std::string& name);
