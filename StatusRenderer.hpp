#pragma once

#include <string>

class StatusRenderer {
public:
    static std::string render(const std::string& status, bool color = true);
};
