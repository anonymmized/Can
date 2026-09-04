#pragma once

#include "LinkData.hpp"
#include <string>

const std::string PREFIX = "vless://";

class LinkParser {
    private:
        LinkData linkData;
        std::string getUuid();
        std::string getHost();
        std::uint16_t getPort();
        std::optional<std::string> getParameter(const std::string& parameterName);
        void checkLink(const std::string& link);
    public:
        LinkData getParsedLink(const std::string& cliArgument);
};
