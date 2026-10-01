#include "vcam/config/config_file.hpp"

#include <fstream>
#include <sstream>

namespace vcam {

namespace {

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// Removes a "# comment" that is not inside quotes.
std::string strip_comment(const std::string& line) {
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string unquote(const std::string& value) {
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

}  // namespace

Result<std::vector<ConfigEntry>> parse_config_text(const std::string& text) {
    std::vector<ConfigEntry> entries;
    std::istringstream lines(text);
    std::string raw;
    std::string section;
    int number = 0;

    while (std::getline(lines, raw)) {
        ++number;
        const std::string without_comment = strip_comment(raw);
        const std::string content = trim(without_comment);
        if (content.empty()) {
            continue;
        }
        const bool indented = !without_comment.empty() && (without_comment[0] == ' ' || without_comment[0] == '\t');
        const size_t colon = content.find(':');
        if (colon == std::string::npos || content[0] == '-') {
            return Status(StatusCode::InvalidArgument,
                          "config line " + std::to_string(number) + ": expected 'key: value' (lists are not supported)");
        }
        const std::string key = trim(content.substr(0, colon));
        const std::string value = unquote(trim(content.substr(colon + 1)));

        if (!indented) {
            if (!value.empty()) {
                return Status(StatusCode::InvalidArgument,
                              "config line " + std::to_string(number) + ": top-level '" + key +
                                  "' must be a section (e.g. 'output:' followed by indented keys)");
            }
            section = key;
            continue;
        }
        if (section.empty()) {
            return Status(StatusCode::InvalidArgument,
                          "config line " + std::to_string(number) + ": indented key outside a section");
        }
        entries.push_back(ConfigEntry{section + "." + key, value, number});
    }
    return entries;
}

Status load_config_file(const std::string& path, Config& config) {
    std::ifstream file(path);
    if (!file) {
        return Status(StatusCode::NotFound, "cannot read config file '" + path + "'");
    }
    std::stringstream content;
    content << file.rdbuf();

    auto entries = parse_config_text(content.str());
    if (!entries.ok()) {
        return Status(entries.status().code(), path + ": " + entries.status().message());
    }
    for (const ConfigEntry& entry : entries.value()) {
        Status status = apply_setting(config, entry.key, entry.value);
        if (!status.ok()) {
            return Status(status.code(), path + " line " + std::to_string(entry.line) + ": " + status.message());
        }
    }
    return Status::ok_status();
}

}  // namespace vcam
