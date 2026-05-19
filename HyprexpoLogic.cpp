#include "HyprexpoLogic.hpp"

#include "HyprexpoConfig.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>

namespace Hyprexpo {

std::string trimString(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

std::vector<std::string> splitCommaList(const std::string& value) {
    std::vector<std::string> entries;
    size_t                   start = 0;

    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos)
            comma = value.size();

        entries.push_back(trimString(value.substr(start, comma - start)));

        if (comma == value.size())
            break;
        start = comma + 1;
    }

    return entries;
}

int clampGridColumns(int columns) {
    return std::clamp(columns, HyprexpoConfig::COLUMNS_MIN, HyprexpoConfig::COLUMNS_MAX);
}

int tileIndexFromPoint(double x, double y, double width, double height, int sideLength) {
    if (width <= 0 || height <= 0 || sideLength <= 0)
        return -1;

    const int safeSide = clampGridColumns(sideLength);
    const int hx       = std::clamp(static_cast<int>(x / width * safeSide), 0, safeSide - 1);
    const int hy       = std::clamp(static_cast<int>(y / height * safeSide), 0, safeSide - 1);
    return hx + hy * safeSide;
}

std::string fallbackTokenForVisibleIndex(int visibleIndex) {
    if (visibleIndex < 0)
        return "";
    if (visibleIndex < 9)
        return std::to_string(visibleIndex + 1);
    if (visibleIndex == 9)
        return "0";
    if (visibleIndex < 36)
        return std::string(1, static_cast<char>('a' + visibleIndex - 10));

    return "";
}

int fallbackTokenToVisibleIndex(const std::string& token) {
    const auto normalized = lowerString(trimString(token));
    if (normalized.size() != 1)
        return -1;

    const char c = normalized[0];
    if (c >= '1' && c <= '9')
        return c - '1';
    if (c == '0')
        return 9;
    if (c >= 'a' && c <= 'z')
        return 10 + c - 'a';

    return -1;
}

static int hexTo(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static bool parseHexByte(const std::string& value, size_t index, int& out) {
    if (index + 1 >= value.size())
        return false;

    const int hi = hexTo(value[index]);
    const int lo = hexTo(value[index + 1]);
    if (hi < 0 || lo < 0)
        return false;

    out = (hi << 4) | lo;
    return true;
}

bool parseHexRGBA8(const std::string& value, SColorRGBA& out) {
    const std::string hex = trimString(value);
    if (hex.size() != 8)
        return false;

    int r = 0, g = 0, b = 0, a = 0;
    if (!parseHexByte(hex, 0, r) || !parseHexByte(hex, 2, g) || !parseHexByte(hex, 4, b) || !parseHexByte(hex, 6, a))
        return false;

    out = SColorRGBA{r / 255.F, g / 255.F, b / 255.F, a / 255.F};
    return true;
}

static bool parseHexARGB8(const std::string& value, SColorRGBA& out) {
    const std::string hex = trimString(value);
    if (hex.size() != 8)
        return false;

    int a = 0, r = 0, g = 0, b = 0;
    if (!parseHexByte(hex, 0, a) || !parseHexByte(hex, 2, r) || !parseHexByte(hex, 4, g) || !parseHexByte(hex, 6, b))
        return false;

    out = SColorRGBA{r / 255.F, g / 255.F, b / 255.F, a / 255.F};
    return true;
}

bool parseSolidColorSpec(const std::string& value, SColorRGBA& out) {
    std::string spec = trimString(value);
    if (spec.empty())
        return false;

    const std::string lowered = lowerString(spec);
    if (lowered.starts_with("rgb(") && spec.ends_with(")")) {
        const auto hex = spec.substr(4, spec.size() - 5);
        return parseHexRGBA8(hex + "ff", out);
    }

    if (lowered.starts_with("rgba(") && spec.ends_with(")")) {
        const auto hex = spec.substr(5, spec.size() - 6);
        return parseHexRGBA8(hex, out);
    }

    if (lowered.starts_with("0x"))
        spec = spec.substr(2);

    return parseHexARGB8(spec, out);
}

SGradientSpec parseGradientSpec(const std::string& value) {
    SGradientSpec spec;
    std::string   normalized = value;
    normalized.erase(std::remove(normalized.begin(), normalized.end(), ','), normalized.end());

    const auto p1 = normalized.find("rgba(");
    const auto p2 = normalized.find("rgba(", p1 == std::string::npos ? 0 : p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos)
        return spec;

    const auto e1 = normalized.find(')', p1);
    const auto e2 = normalized.find(')', p2);
    if (e1 == std::string::npos || e2 == std::string::npos)
        return spec;

    if (!parseHexRGBA8(normalized.substr(p1 + 5, e1 - (p1 + 5)), spec.c1) || !parseHexRGBA8(normalized.substr(p2 + 5, e2 - (p2 + 5)), spec.c2))
        return spec;

    const auto deg = normalized.find("deg", e2);
    if (deg != std::string::npos) {
        size_t begin = normalized.rfind(' ', deg);
        if (begin == std::string::npos)
            begin = e2 + 1;
        else
            begin += 1;

        float parsed = 0.F;
        const auto angle = trimString(normalized.substr(begin, deg - begin));
        const auto res   = std::from_chars(angle.data(), angle.data() + angle.size(), parsed);
        if (res.ec == std::errc{} && res.ptr == angle.data() + angle.size())
            spec.angleDeg = parsed;
    }

    spec.valid = true;
    return spec;
}

bool isGradientBorderSpec(const std::string& value) {
    const auto first = value.find("rgba(");
    return first != std::string::npos && value.find("rgba(", first + 1) != std::string::npos;
}

static std::vector<std::string> splitWhitespace(const std::string& value) {
    std::vector<std::string> tokens;
    size_t                   cursor = 0;

    while (cursor < value.size()) {
        while (cursor < value.size() && std::isspace(static_cast<unsigned char>(value[cursor])))
            ++cursor;
        const size_t begin = cursor;
        while (cursor < value.size() && !std::isspace(static_cast<unsigned char>(value[cursor])))
            ++cursor;
        if (begin < cursor)
            tokens.push_back(value.substr(begin, cursor - begin));
    }

    return tokens;
}

SWorkspaceMethodSpec parseWorkspaceMethodSpec(const std::string& method) {
    SWorkspaceMethodSpec spec;
    const auto           tokens = splitWhitespace(method);

    if (tokens.size() != 2) {
        spec.error = "expected '<center|first> <workspace>'";
        return spec;
    }

    const auto mode = lowerString(tokens[0]);
    if (mode == "center")
        spec.mode = EWorkspaceMethodMode::Center;
    else if (mode == "first")
        spec.mode = EWorkspaceMethodMode::First;
    else {
        spec.error = "expected workspace method 'center' or 'first'";
        return spec;
    }

    if (tokens[1].empty()) {
        spec.error = "workspace token cannot be empty";
        return spec;
    }

    spec.workspace = tokens[1];
    spec.valid     = true;
    return spec;
}

SWorkspaceMethodSpec resolveWorkspaceMethodForMonitor(const std::string& config, const std::string& monitorName) {
    const std::string trimmed = trimString(config);
    if (trimmed.empty())
        return parseWorkspaceMethodSpec(HyprexpoConfig::WORKSPACE_METHOD_DEFAULT);

    std::string globalFallback;
    for (const auto& entry : splitCommaList(trimmed)) {
        if (entry.empty())
            continue;

        const auto tokens = splitWhitespace(entry);
        if (tokens.size() == 3) {
            if (tokens[0] == monitorName)
                return parseWorkspaceMethodSpec(tokens[1] + " " + tokens[2]);
            continue;
        }

        if (tokens.size() == 2 && globalFallback.empty())
            globalFallback = entry;
    }

    if (!globalFallback.empty())
        return parseWorkspaceMethodSpec(globalFallback);

    auto invalid = parseWorkspaceMethodSpec(trimmed);
    if (!invalid.valid && invalid.error.empty())
        invalid.error = "invalid workspace method config";
    return invalid;
}

}
