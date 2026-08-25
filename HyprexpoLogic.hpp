#pragma once

#include <cstddef>
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

enum class ENumberKeyMode {
    Workspace,
    Index,
    Passthrough,
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

struct SGridShape {
    int cols = 1;
    int rows = 1;
};

struct STileLayout {
    SRect box;
    int   row = -1;
    int   col = -1;
};

struct SDropIntentInput {
    bool   targetValid     = false;
    SPoint pointerLocal    = {};
    SRect  targetTileLocal = {};
    SSize  workspaceSize   = {};
    SSize  windowSize      = {};
    SPoint grabOffset      = {};
    double minProxySize    = 24.0;
};

struct SDropIntentGeometry {
    bool   valid               = false;
    SPoint targetWorkspacePoint = {};
    SRect  targetProxyLocal     = {};
};

struct SGestureConfig {
    int         fingers = 0;
    std::string direction;
    bool        directionValid = false;
};

struct SGestureSyncDecision {
    bool        registerGesture = false;
    std::string error;
};

std::string trimString(std::string value);
std::string lowerString(std::string value);
std::vector<std::string> splitCommaList(const std::string& value);

SGridShape               computeDynamicGridShape(int visibleCount);
std::optional<std::vector<int64_t>> expandDynamicWorkspaceIDs(const std::vector<int64_t>& workspaceIDs, bool fillGaps, std::size_t maxExpandedWorkspaces);
SSize                    aspectCorrectTileSize(double screenW, double screenH, int cols, int rows, double gap);
STileLayout              computeTileLayout(int index, int visibleCount, SGridShape shape, SSize total, double gap, bool centerPartialRows);
int                      tileIndexAtPoint(double x, double y, int visibleCount, SGridShape shape, SSize total, double gap, bool centerPartialRows);

int                      clampGridColumns(int columns);
int                      gridColumnsToIncludeWorkspace(int configuredColumns, int firstWorkspaceID, int activeWorkspaceID, int maxColumns);
int                      tileIndexFromPoint(double x, double y, double width, double height, int sideLength);
int                      numberKeyToVisibleIndex(int number);
ENumberKeyMode           numberKeyModeFromString(const std::string& mode);
SDropIntentGeometry      computeDropIntentGeometry(const SDropIntentInput& input);

SGestureSyncDecision     evaluateGestureSync(const SGestureConfig& config);

std::string              decodeConfigString(const void* dataptr, bool underlyingIsStdString, const std::string& fallback);

std::string              fallbackTokenForVisibleIndex(int visibleIndex);
int                      fallbackTokenToVisibleIndex(const std::string& token);

bool                     parseHexRGBA8(const std::string& value, SColorRGBA& out);
bool                     parseSolidColorSpec(const std::string& value, SColorRGBA& out);
SGradientSpec            parseGradientSpec(const std::string& value);
bool                     isGradientBorderSpec(const std::string& value);
bool                     shouldShowWorkspaceLabel(bool labelEnabled, const std::string& labelShow, bool isHovered, bool isFocused, bool isCurrent);
std::string              resolveBorderSpec(const std::string& modernSpec, const std::string& legacySpec);

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
