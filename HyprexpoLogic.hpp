#pragma once

#include <cstdint>
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

// --- all_monitors (consolidated all-workspaces overview) pure decisions -------------------------

/**
 * @brief Global workspace IDs that fill the consolidated grid's cells.
 *
 * In all_monitors mode every cell maps to a global workspace by ID rather than to the invoking
 * monitor's contiguous-from-active range, so the grid shows all workspaces regardless of owner.
 *
 * @param tileCount Number of grid cells (sideLength * sideLength, derived from the columns config).
 * @return {1, 2, ..., tileCount}.
 */
std::vector<int64_t>     allMonitorsCellWorkspaceIDs(int tileCount);

/**
 * @brief Cell index of the workspace the overview opened on, within the 1..tileCount enumeration.
 *
 * Clarity helper for the capture-loop currentid seed; not correctness-load-bearing.
 *
 * @param startedOnID The active workspace ID when the overview opened.
 * @param tileCount   Number of grid cells.
 * @return Zero-based index of startedOnID in {1..tileCount}, or 0 if it falls outside that range.
 */
int                      allMonitorsOpenIndex(int64_t startedOnID, int tileCount);

enum class EClickIntent {
    FocusOwner,    // path (a): focus the workspace's owner monitor and switch there
    PullToCurrent, // path (b): pull the workspace onto the current monitor
};

/**
 * @brief Resolve which click action a release maps to in all_monitors mode.
 *
 * @param rightButton True if the released button was the right mouse button.
 * @param shiftHeld    True if Shift was held during the release.
 * @return PullToCurrent for right-button or Shift+left; FocusOwner for a plain left click.
 */
EClickIntent             resolveClickIntent(bool rightButton, bool shiftHeld);

}
