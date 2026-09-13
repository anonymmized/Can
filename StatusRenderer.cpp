#include "StatusRenderer.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace {
std::vector<std::string> splitLines(const std::string& value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < value.size()) {
        const auto end = value.find('\n', start);
        result.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

std::string ansi(bool color, const std::string& code, const std::string& text) {
    return color ? "\033[" + code + "m" + text + "\033[0m" : text;
}

std::string gradientLine(std::size_t width, bool color) {
    std::string line;
    line.reserve(width * 20);
    for (std::size_t i = 0; i < width; ++i) {
        const int red = static_cast<int>(40 + (210 * i) / std::max<std::size_t>(1, width - 1));
        const int green = static_cast<int>(190 - (120 * i) / std::max<std::size_t>(1, width - 1));
        const int blue = static_cast<int>(255 - (40 * i) / std::max<std::size_t>(1, width - 1));
        std::ostringstream code;
        code << "38;2;" << red << ';' << green << ';' << blue;
        line += ansi(color, code.str(), "━");
    }
    return line;
}
}

std::string StatusRenderer::render(const std::string& status, bool color) {
    const auto lines = splitLines(status);
    const bool inactive = status.rfind("No active background session", 0) == 0;
    const bool stopping = status.rfind("Stop requested", 0) == 0;
    const std::string title = inactive ? "NO ACTIVE SESSION" : stopping ? "STOPPING SESSION" : "CONNECTION STATUS";
    const std::string icon = inactive ? "○" : stopping ? "◌" : "●";
    const std::string stateColor = inactive ? "38;5;245" : stopping ? "38;5;214" : "38;5;82";
    std::ostringstream output;
    output << '\n' << gradientLine(58, color) << '\n';
    output << "  " << ansi(color, stateColor, icon) << ' ' << ansi(color, "1;38;5;255", title) << '\n';
    output << gradientLine(58, color) << '\n';
    if (inactive) {
        output << "\n  " << ansi(color, "38;5;245", "There is no running background connection.") << '\n';
    } else {
        for (std::size_t i = 1; i < lines.size(); ++i) {
            const auto line = trim(lines[i]);
            if (line.empty() || line == "Internet connectivity is not checked by status.") continue;
            const auto separator = line.find(':');
            if (separator == std::string::npos) {
                output << "  " << line << '\n';
                continue;
            }
            const auto key = line.substr(0, separator);
            const auto value = trim(line.substr(separator + 1));
            std::ostringstream label;
            label << std::left << std::setw(12) << key;
            output << "  " << ansi(color, "38;5;244", label.str())
                   << ansi(color, "38;5;255", value) << '\n';
        }
        output << '\n' << "  " << ansi(color, "38;5;244", "Connectivity is not checked by status.") << '\n';
    }
    output << gradientLine(58, color) << '\n';
    return output.str();
}
