#include "LinkParser.hpp"

#include <optional>
#include <string>

void LinkParser::checkLink(const std::string& link) {
    if (link.size() <= PREFIX.size()) {
        throw std::invalid_argument("VLESS URL is too short");
    } 
    if (link.compare(0, PREFIX.size(), PREFIX) != 0) {
        throw std::invalid_argument("Link is not VLESS");
    }
    linkData.rawUrl = link;
}

std::string LinkParser::getUuid() {
    size_t uuidStart = PREFIX.size();
    size_t uuidEnd = linkData.rawUrl.find('@', uuidStart);
    if (linkData.rawUrl.compare(0, PREFIX.size(), PREFIX) != 0) {
        throw std::invalid_argument("Link is not VLESS");
    }
    if (uuidEnd == std::string::npos || uuidEnd == uuidStart) {
        throw std::invalid_argument("UUID not found");
    }
    return linkData.rawUrl.substr(uuidStart, uuidEnd - uuidStart);
}

std::string LinkParser::getHost() {
    size_t atPosition = linkData.rawUrl.find('@');
    if (atPosition == std::string::npos) {
        throw std::invalid_argument("Host not found");
    }
    size_t hostStart = atPosition + 1;
    size_t hostEnd = linkData.rawUrl.find(':', hostStart);
    if (hostEnd == std::string::npos || hostEnd == hostStart) {
        throw std::invalid_argument("Host not found");
    }
    return linkData.rawUrl.substr(hostStart, hostEnd - hostStart);
}

std::uint16_t LinkParser::getPort() {
    size_t atPosition = linkData.rawUrl.find('@');
    if (atPosition == std::string::npos) {
        throw std::invalid_argument("Host not found");
    }
    size_t colonPosition = linkData.rawUrl.find(':', atPosition + 1);
    if (colonPosition == std::string::npos) {
        throw std::invalid_argument("Port not found");
    }
    size_t portStart = colonPosition + 1;
    size_t portEnd = linkData.rawUrl.find_first_of("?#", portStart);
    if (portEnd == std::string::npos) {
        portEnd = linkData.rawUrl.size();
    }
    if (portEnd == portStart) {
        throw std::invalid_argument("Port is empty");
    }
    std::string portText = linkData.rawUrl.substr(portStart, portEnd - portStart);
    size_t parsedCharacters = 0;
    unsigned long port = std::stoul(portText, &parsedCharacters);
    if (parsedCharacters != portText.size() || port < 1 || port > 65535) {
        throw std::out_of_range("Invalid port");
    }
    return static_cast<std::uint16_t>(port);
}

std::optional<std::string> LinkParser::getParameter(const std::string& parameterName) {
    std::string marker = "?" + parameterName + "=";
    size_t markerPosition = linkData.rawUrl.find(marker);
    if (markerPosition == std::string::npos) {
        marker = "&" + parameterName + "=";
        markerPosition = linkData.rawUrl.find(marker);
    }
    if (markerPosition == std::string::npos) {
        return std::nullopt;
    }
    size_t parameterStart = markerPosition + marker.size();
    size_t parameterEnd = linkData.rawUrl.find_first_of("&#", parameterStart);
    if (parameterEnd == parameterStart) {
        throw std::invalid_argument("Parameter '" + parameterName + "' is empty");
    }
    return linkData.rawUrl.substr(parameterStart, parameterEnd - parameterStart);
}

LinkData LinkParser::getParsedLink(const std::string& cliArgument) {
    checkLink(cliArgument);
    linkData.uuid = getUuid();
    linkData.host = getHost();
    linkData.port = getPort();
    linkData.encryption = getParameter("encryption").value_or("none");
    linkData.flow = getParameter("flow").value_or("");
    linkData.security = getParameter("security").value_or("none");
    linkData.sni = getParameter("sni").value_or(linkData.host);
    linkData.fingerprint = getParameter("fp").value_or("");
    linkData.publicKey = getParameter("pbk").value_or("");
    linkData.shortId = getParameter("sid").value_or("");
    linkData.transport = getParameter("type").value_or("tcp");
    return linkData;
}


