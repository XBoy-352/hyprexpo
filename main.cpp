#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#include <hyprland/src/managers/SeatManager.hpp>

#include <lua.hpp>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <charconv>
#include <cctype>

#include "globals.hpp"
#include "overview.hpp"
#include "ExpoGesture.hpp"
#include <hyprland/src/event/EventBus.hpp>

// Methods
inline CFunctionHook* g_pRenderWorkspaceHook = nullptr;
inline CFunctionHook* g_pAddDamageHookA      = nullptr;
inline CFunctionHook* g_pAddDamageHookB      = nullptr;
typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, timespec*, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);

static bool g_unloading = false;
static SP<Config::Values::CStringValue> g_pCancelKeyConfig;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool renderingOverview = false;

// forward declarations for new dispatchers
static SDispatchResult onExpoDispatcher(std::string arg);
static SDispatchResult onKbFocusDispatcher(std::string arg);
static SDispatchResult onKbConfirmDispatcher(std::string arg);
static SDispatchResult onKbSelectNumberDispatcher(std::string arg);
static SDispatchResult onKbSelectTokenDispatcher(std::string arg);
static SDispatchResult onKbSelectIndexDispatcher(std::string arg);
static SDispatchResult registerExpoGesture(int fingerCount, const std::string& directionName, const std::string& action, const std::string& mods, float deltaScale, bool disableInhibit);

static std::string trimString(std::string value) {
    while (!value.empty() && std::isspace((unsigned char)value.front()))
        value.erase(value.begin());
    while (!value.empty() && std::isspace((unsigned char)value.back()))
        value.pop_back();
    return value;
}

static std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

static bool parseStrictInteger(const std::string& value, int& out) {
    const std::string trimmed = trimString(value);
    if (trimmed.empty())
        return false;

    const char* begin = trimmed.data();
    const char* end   = begin + trimmed.size();
    int         parsed = 0;
    const auto  result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end)
        return false;

    out = parsed;
    return true;
}

static bool isCancelKeyDisabled(const std::string& keyName) {
    const std::string key = lowerString(keyName);
    return key.empty() || key == "none" || key == "disabled" || key == "disable" || key == "off";
}

static bool keyNameMatchesKeysym(const std::string& keyName, xkb_keysym_t keysym) {
    if (keyName.empty())
        return false;

    const auto configuredKeysym = xkb_keysym_from_name(keyName.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (configuredKeysym == XKB_KEY_NoSymbol)
        return false;

    return xkb_keysym_to_lower(keysym) == xkb_keysym_to_lower(configuredKeysym);
}

static bool matchesCancelKey(xkb_keysym_t keysym) {
    std::string keyConfig = g_pCancelKeyConfig ? g_pCancelKeyConfig->value() : "escape";
    size_t      start     = 0;

    while (start <= keyConfig.size()) {
        size_t comma = keyConfig.find(',', start);
        if (comma == std::string::npos)
            comma = keyConfig.size();

        const std::string keyName = trimString(keyConfig.substr(start, comma - start));
        if (isCancelKeyDisabled(keyName))
            return false;
        if (keyNameMatchesKeysym(keyName, keysym))
            return true;

        if (comma == keyConfig.size())
            break;
        start = comma + 1;
    }

    return false;
}

static bool shouldCancelOverview(const IKeyboard::SKeyEvent& event) {
    if (!g_pOverview || event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return false;

    const auto KEYCODE  = event.keycode + 8;
    const auto KEYBOARD = g_pSeatManager->m_keyboard.lock();

    if (KEYBOARD && KEYBOARD->m_xkbState && matchesCancelKey(xkb_state_key_get_one_sym(KEYBOARD->m_xkbState, KEYCODE)))
        return true;
    if (KEYBOARD && KEYBOARD->m_xkbSymState && matchesCancelKey(xkb_state_key_get_one_sym(KEYBOARD->m_xkbSymState, KEYCODE)))
        return true;

    return false;
}

static int luaDispatchResult(lua_State* L, const char* name, const SDispatchResult& result) {
    if (result.success)
        return 0;

    return luaL_error(L, "%s: %s", name, result.error.empty() ? "dispatcher failed" : result.error.c_str());
}

static std::string luaStringArg(lua_State* L, int index, const char* name, const char* defaultValue = "") {
    if (lua_gettop(L) < index || lua_isnil(L, index))
        return defaultValue;

    if (lua_type(L, index) == LUA_TSTRING)
        return lua_tostring(L, index);

    luaL_error(L, "%s: argument %d must be a string", name, index);
    return defaultValue;
}

static std::string luaIntegerArg(lua_State* L, int index, const char* name) {
    if (lua_gettop(L) < index || lua_isnil(L, index)) {
        luaL_error(L, "%s: argument %d must be an integer", name, index);
        return "";
    }

    if (lua_type(L, index) == LUA_TNUMBER) {
        if (!lua_isinteger(L, index)) {
            luaL_error(L, "%s: argument %d must be an integer, not a fractional number", name, index);
            return "";
        }
        return std::to_string(lua_tointeger(L, index));
    }

    if (lua_type(L, index) == LUA_TSTRING) {
        int parsed = 0;
        const std::string value = lua_tostring(L, index);
        if (!parseStrictInteger(value, parsed)) {
            luaL_error(L, "%s: argument %d must be an integer string", name, index);
            return "";
        }
        return trimString(value);
    }

    luaL_error(L, "%s: argument %d must be an integer", name, index);
    return "";
}

static std::string luaTableStringField(lua_State* L, const char* name, const char* field, const char* defaultValue = nullptr) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (defaultValue)
            return defaultValue;
        luaL_error(L, "%s: field '%s' must be a string", name, field);
        return "";
    }

    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        luaL_error(L, "%s: field '%s' must be a string", name, field);
        return "";
    }

    std::string value = lua_tostring(L, -1);
    lua_pop(L, 1);
    return value;
}

static int luaTableIntegerField(lua_State* L, const char* name, const char* field) {
    lua_getfield(L, 1, field);
    if (!lua_isinteger(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "%s: field '%s' must be an integer", name, field);
        return 0;
    }

    const int value = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return value;
}

static float luaTableFloatField(lua_State* L, const char* name, const char* field, float defaultValue) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return defaultValue;
    }

    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "%s: field '%s' must be a number", name, field);
        return defaultValue;
    }

    const float value = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool luaTableBoolField(lua_State* L, const char* name, const char* field, bool defaultValue) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return defaultValue;
    }

    if (lua_type(L, -1) != LUA_TBOOLEAN) {
        lua_pop(L, 1);
        luaL_error(L, "%s: field '%s' must be a boolean", name, field);
        return defaultValue;
    }

    const bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static int luaExpo(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.expo", onExpoDispatcher(luaStringArg(L, 1, "hyprexpo.expo", "toggle")));
}

static int luaKbFocus(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_focus", onKbFocusDispatcher(luaStringArg(L, 1, "hyprexpo.kb_focus")));
}

static int luaKbConfirm(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_confirm", onKbConfirmDispatcher(""));
}

static int luaKbSelectNumber(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_selectn", onKbSelectNumberDispatcher(luaIntegerArg(L, 1, "hyprexpo.kb_selectn")));
}

static int luaKbSelectToken(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_select", onKbSelectTokenDispatcher(luaStringArg(L, 1, "hyprexpo.kb_select")));
}

static int luaKbSelectIndex(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_selecti", onKbSelectIndexDispatcher(luaIntegerArg(L, 1, "hyprexpo.kb_selecti")));
}

static int luaGesture(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    const int         fingers        = luaTableIntegerField(L, "hyprexpo.gesture", "fingers");
    const std::string direction      = luaTableStringField(L, "hyprexpo.gesture", "direction");
    const std::string action         = luaTableStringField(L, "hyprexpo.gesture", "action", "expo");
    const std::string mods           = luaTableStringField(L, "hyprexpo.gesture", "mods", "");
    const float       scale          = luaTableFloatField(L, "hyprexpo.gesture", "scale", 1.0F);
    const bool        disableInhibit = luaTableBoolField(L, "hyprexpo.gesture", "disable_inhibit", false);

    return luaDispatchResult(L, "hyprexpo.gesture", registerExpoGesture(fingers, direction, action, mods, scale, disableInhibit));
}

//
static void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, timespec* now, const CBox& geometry) {
    if (!g_pOverview || renderingOverview || g_pOverview->blockOverviewRendering || g_pOverview->pMonitor != pMonitor)
        ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(thisptr, pMonitor, pWorkspace, now, geometry);
    else
        g_pOverview->render();
}

static void hkAddDamageA(void* thisptr, const CBox& box) {
    const auto PMONITOR = (CMonitor*)thisptr;

    if (!g_pOverview || g_pOverview->pMonitor != PMONITOR->m_self || g_pOverview->blockDamageReporting) {
        ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
        return;
    }

    g_pOverview->onDamageReported();
}

static void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    const auto PMONITOR = (CMonitor*)thisptr;

    if (!g_pOverview || g_pOverview->pMonitor != PMONITOR->m_self || g_pOverview->blockDamageReporting) {
        ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
        return;
    }

    g_pOverview->onDamageReported();
}

static SDispatchResult onExpoDispatcher(std::string arg) {

    if (g_pOverview && g_pOverview->m_isSwiping)
        return {.success = false, .error = "already swiping"};

    if (arg == "select") {
        if (g_pOverview) {
            g_pOverview->selectHoveredWorkspace();
            g_pOverview->close();
        }
        return {};
    }
    if (arg == "toggle") {
        if (g_pOverview)
            g_pOverview->close();
        else {
            renderingOverview = true;
            g_pOverview       = std::make_unique<COverview>(g_pCompositor->getMonitorFromCursor()->m_activeWorkspace);
            renderingOverview = false;
        }
        return {};
    }

    if (arg == "cancel") {
        if (g_pOverview)
            g_pOverview->close(false);
        return {};
    }

    if (arg == "off" || arg == "close" || arg == "disable") {
        if (g_pOverview)
            g_pOverview->close();
        return {};
    }

    if (g_pOverview)
        return {};

    renderingOverview = true;
    g_pOverview       = std::make_unique<COverview>(g_pCompositor->getMonitorFromCursor()->m_activeWorkspace);
    renderingOverview = false;
    return {};
}

static void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(PHANDLE, "[hyprexpo] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

static SDispatchResult registerExpoGesture(int fingerCount, const std::string& directionName, const std::string& action, const std::string& mods, float deltaScale, bool disableInhibit) {
    if (g_unloading)
        return {};

    if (fingerCount <= 1 || fingerCount >= 10)
        return {.success = false, .error = std::format("invalid fingers '{}', expected 2-9", fingerCount)};

    const auto direction = g_pTrackpadGestures->dirForString(directionName);
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("invalid direction '{}'", directionName)};

    uint32_t modMask = 0;
    if (!mods.empty())
        modMask = g_pKeybindManager->stringToModMask(mods);

    deltaScale = std::clamp(deltaScale, 0.1F, 10.F);

    std::expected<void, std::string> result;
    if (action == "expo")
        result = g_pTrackpadGestures->addGesture(makeUnique<CExpoGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "unset")
        result = g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
    else
        return {.success = false, .error = std::format("invalid action '{}', expected expo|unset", action)};

    if (!result)
        return {.success = false, .error = result.error()};

    return {};
}

static void addConfigValue(SP<Config::Values::IValue> value) {
    HyprlandAPI::addConfigValueV2(PHANDLE, value);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH = __hyprland_api_get_hash();

    if (HASH != __hyprland_api_get_client_hash()) {
        failNotif("Version mismatch (headers ver is not equal to running hyprland ver)");
        throw std::runtime_error("[he] Version mismatch");
    }

    auto FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspace");
    if (FNS.empty()) {
        failNotif("no fns for hook renderWorkspace");
        throw std::runtime_error("[he] No fns for hook renderWorkspace");
    }

    g_pRenderWorkspaceHook = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkRenderWorkspace);

    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "addDamageEPK15pixman_region32");
    if (FNS.empty()) {
        failNotif("no fns for hook addDamageEPK15pixman_region32");
        throw std::runtime_error("[he] No fns for hook addDamageEPK15pixman_region32");
    }

    g_pAddDamageHookB = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkAddDamageB);

    FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
    if (FNS.empty()) {
        failNotif("no fns for hook _ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
        throw std::runtime_error("[he] No fns for hook _ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
    }

    g_pAddDamageHookA = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkAddDamageA);

    bool success = g_pRenderWorkspaceHook->hook();
    success      = success && g_pAddDamageHookA->hook();
    success      = success && g_pAddDamageHookB->hook();

    if (!success) {
        failNotif("Failed initializing hooks");
        throw std::runtime_error("[he] Failed initializing hooks");
    }

    static auto P = Event::bus()->m_events.render.pre.listen([](PHLMONITOR pMonitor) {
        if (!g_pOverview)
            return;
        g_pOverview->onPreRender();
    });

    static auto PKEY = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        if (!shouldCancelOverview(event))
            return;

        info.cancelled = true;
        g_pOverview->close(false);
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:expo", ::onExpoDispatcher);

    // keyboard navigation dispatchers
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_focus", ::onKbFocusDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_confirm", ::onKbConfirmDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_selectn", ::onKbSelectNumberDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_select", ::onKbSelectTokenDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_selecti", ::onKbSelectIndexDispatcher);

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "expo", ::luaExpo);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_focus", ::luaKbFocus);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_confirm", ::luaKbConfirm);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_selectn", ::luaKbSelectNumber);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_select", ::luaKbSelectToken);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_selecti", ::luaKbSelectIndex);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "gesture", ::luaGesture);

    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:columns", "columns", 3));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:gaps_in", "inner gaps", 5));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyprexpo:bg_col", "background color", 0xFF111111));
    // Supports both global and per-monitor formats:
    // Global: "center current" or "first 1"
    // Per-monitor with comma delimiter: "DP-1 first 1, HDMI-1 center current"
    // Mixed: "DP-1 first 1, center current" (DP-1 uses first 1, others use center current)
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:workspace_method", "workspace method", "center current"));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:skip_empty", "skip empty workspaces", 0));

    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:gesture_distance", "gesture distance", 200));
    g_pCancelKeyConfig = makeShared<Config::Values::CStringValue>("plugin:hyprexpo:cancel_key", "cancel key", "escape");
    addConfigValue(g_pCancelKeyConfig);
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:show_cursor", "show cursor during overview", 1));

    // keyboard navigation + styling
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:keynav_enable", "key navigation enable", 1));
    // Border configuration - supports both solid colors and gradients
    // Solid: rgb(rrggbb) or 0xAARRGGBB
    // Gradient: rgba(rrggbbaa) rgba(rrggbbaa) 45deg
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:border_width", "border width", 2));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:border_color", "border color", ""));           // default border (unused tiles)
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:border_color_current", "current border color", "rgb(66ccff)"));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:border_color_focus", "focus border color", "rgb(ffcc66)"));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:border_color_hover", "hover border color", "rgb(aabbcc)"));
    // Deprecated but supported for backwards compatibility
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:border_style", "border style", "simple"));     // ignored, auto-detected from format
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_enable", "label enable", 1));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyprexpo:label_color", "label color", 0xFFFFFFFF));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_font_size", "label font size", 16));
    // label_text_mode: token (default) | id | index
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:label_text_mode", "label text mode", "token"));
    // Optional override map for up to 50 tokens, comma-separated. Empty entries allowed.
    // Example: "1,2,3,4,5,6,7,8,9,0,!,@,#,$,%,^,&,*,(,),a,..."
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:label_token_map", "label token map", ""));

    // tile rounding (rounded corners for workspace previews)
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:tile_rounding", "tile rounding", 0));
    addConfigValue(makeShared<Config::Values::CFloatValue>("plugin:hyprexpo:tile_rounding_power", "tile rounding power", 2.0F));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:tile_rounding_focus", "focus tile rounding", -1));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:tile_rounding_current", "current tile rounding", -1));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:tile_rounding_hover", "hover tile rounding", -1));

    // (shadows moved to feature/shadows branch)
    // defaults: center/middle within the label container
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:label_position", "label position", "center"));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_offset_x", "label offset x", 0));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_offset_y", "label offset y", 0));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:selection_label_enable", "selection label enable", 0));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:selection_label_token_map", "selection label token map", "a,s,d,f,g,q,w,e,r,t,z,x,c,v,b"));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:selection_label_position", "selection label position", "top-right"));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:selection_label_offset_x", "selection label offset x", 6));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:selection_label_offset_y", "selection label offset y", 6));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyprexpo:selection_label_color", "selection label color", 0xFFFFCC66));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:label_show", "label show", "always"));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyprexpo:label_color_default", "default label color", 0xFFFFFFFF));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyprexpo:label_color_hover", "hover label color", 0xFFEEEEEE));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyprexpo:label_color_focus", "focus label color", 0xFFFFCC66));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyprexpo:label_color_current", "current label color", 0xFF66CCFF));
    addConfigValue(makeShared<Config::Values::CFloatValue>("plugin:hyprexpo:label_scale_hover", "hover label scale", 1.0F));
    addConfigValue(makeShared<Config::Values::CFloatValue>("plugin:hyprexpo:label_scale_focus", "focus label scale", 1.0F));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_bg_enable", "label background enable", 1));
    addConfigValue(makeShared<Config::Values::CColorValue>("plugin:hyprexpo:label_bg_color", "label background color", 0x88000000));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_bg_rounding", "label background rounding", 8));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:label_bg_shape", "label background shape", "circle"));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_padding", "label padding", 8));
    // label font styling and pixel snapping
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:label_font_family", "label font family", "sans"));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_font_bold", "label font bold", 0));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_font_italic", "label font italic", 0));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_text_underline", "label text underline", 0));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_text_strikethrough", "label text strikethrough", 0));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_pixel_snap", "label pixel snap", 1));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_center_adjust_x", "label center adjust x", 0));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:label_center_adjust_y", "label center adjust y", 0));
    // gaps
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:gaps_out", "outer gaps", 0));
    // Deprecated: use border_color_* instead (supports both solid and gradient)
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:border_grad_current", "current border gradient", ""));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:border_grad_focus", "focus border gradient", ""));
    addConfigValue(makeShared<Config::Values::CStringValue>("plugin:hyprexpo:border_grad_hover", "hover border gradient", ""));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:keynav_wrap_h", "key navigation horizontal wrap", 1));
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:keynav_wrap_v", "key navigation vertical wrap", 1));
    // default off: spatial moves by default
    addConfigValue(makeShared<Config::Values::CIntValue>("plugin:hyprexpo:keynav_reading_order", "key navigation reading order", 0));

    HyprlandAPI::reloadConfig();

    return {"hyprexpo-plus", "hyprexpo+ with keyboard selection, labels, and borders", "sandwich", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("COverviewPassElement");

    g_unloading = true;

    Config::mgr()->reload(); // we need to reload now to clear all the gestures
    g_pCancelKeyConfig.reset();
}

//
// New dispatchers for keyboard navigation
//

static SDispatchResult onKbFocusDispatcher(std::string arg) {
    if (!g_pOverview)
        return {};

    if (arg == "left" || arg == "right" || arg == "up" || arg == "down") {
        g_pOverview->onKbMoveFocus(arg);
        return {};
    }

    return {.success = false, .error = "invalid arg. expected left|right|up|down"};
}

static SDispatchResult onKbConfirmDispatcher(std::string arg) {
    if (!g_pOverview)
        return {};

    g_pOverview->onKbConfirm();
    return {};
}

static SDispatchResult onKbSelectNumberDispatcher(std::string arg) {
    if (!g_pOverview)
        return {};

    arg = trimString(arg);
    if (arg.empty())
        return {.success = false, .error = "missing number"};

    int num = -1;
    if (!parseStrictInteger(arg, num))
        return {.success = false, .error = "invalid number"};

    g_pOverview->onKbSelectNumber(num);
    return {};
}

static SDispatchResult onKbSelectTokenDispatcher(std::string arg) {
    if (!g_pOverview)
        return {};
    arg = trimString(arg);
    if (!g_pOverview->selectVisibleToken(arg))
        return {.success = false, .error = "no visible workspace for token"};
    g_pOverview->close();
    return {};
}

static SDispatchResult onKbSelectIndexDispatcher(std::string arg) {
    if (!g_pOverview)
        return {};
    arg = trimString(arg);
    int idx = -1;
    if (!parseStrictInteger(arg, idx))
        idx = -1;
    if (idx <= 0)
        return {.success = false, .error = "invalid index (expected >= 1)"};
    // convert to 0-based visible index
    g_pOverview->onKbSelectToken(idx - 1);
    return {};
}
