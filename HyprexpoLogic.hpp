#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Hyprexpo {

struct SColorRGBA {
    float r = 0.F;
    float g = 0.F;
    float b = 0.F;
    float a = 1.F;
};

struct SGradientSpec {
    SColorRGBA c1;
    SColorRGBA c2;
    float      angleDeg = 0.F;
    bool       valid    = false;
};

enum class EWorkspaceMethodMode {
    Center,
    First,
};

struct SWorkspaceMethodSpec {
    bool                 valid = false;
    EWorkspaceMethodMode mode  = EWorkspaceMethodMode::Center;
    std::string          workspace;
    std::string          error;
};

struct SPoint {
    double x = 0.0;
    double y = 0.0;
};

struct SSize {
    double w = 0.0;
    double h = 0.0;
};

struct SRect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

struct SDropIntentInput {
    bool   targetValid        = false;
    SPoint pointerLocal       = {};
    SRect  targetTileLocal    = {};
    SSize  workspaceSize      = {};
    SSize  windowSize         = {};
    SPoint grabOffset         = {};
    double minProxySize       = 24.0;
};

struct SDropIntentGeometry {
    bool   valid                = false;
    SPoint targetWorkspacePoint = {};
    SRect  targetProxyLocal     = {};
};

std::string              trimString(std::string value);
std::string              lowerString(std::string value);
std::vector<std::string> splitCommaList(const std::string& value);

int                      clampGridColumns(int columns);
int                      tileIndexFromPoint(double x, double y, double width, double height, int sideLength);
SDropIntentGeometry      computeDropIntentGeometry(const SDropIntentInput& input);

std::string              fallbackTokenForVisibleIndex(int visibleIndex);
int                      fallbackTokenToVisibleIndex(const std::string& token);

bool                     parseHexRGBA8(const std::string& value, SColorRGBA& out);
bool                     parseSolidColorSpec(const std::string& value, SColorRGBA& out);
SGradientSpec            parseGradientSpec(const std::string& value);
bool                     isGradientBorderSpec(const std::string& value);

SWorkspaceMethodSpec     parseWorkspaceMethodSpec(const std::string& method);
SWorkspaceMethodSpec     resolveWorkspaceMethodForMonitor(const std::string& config, const std::string& monitorName);

}
