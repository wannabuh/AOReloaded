// settings.cpp — AOReloaded persistent settings via .ini file.
//
// Architecture:
//   - A static table of SettingDef entries defines every AOReloaded setting
//     (name, type, default value).
//   - SettingsInit() reads AOReloaded.ini into each entry's `current` field.
//   - SettingsRegisterAll() creates DValues from those values.
//   - SettingsInstallHook() detours SetDValue so that any change to an
//     AOReloaded DValue is immediately persisted to the .ini.
//
// The .ini uses the [AOReloaded] section. Bools are stored as 0/1, ints
// as decimal integers. We use the Win32 Private Profile API which handles
// file locking and atomic writes.

#include "core/settings.h"
#include "core/gui_overlay.h"
#include "core/renderer_settings.h"
#include "core/fps_cap.h"
#include "core/logging.h"
#include "ao/game_api.h"
#include "ao/types.h"
#include "hooks/hook_engine.h"
#include "hooks/view_distance.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace aor {

// ── Setting definitions ────────────────────────────────────────────────

enum class SettingType : uint8_t { Bool, Int };

struct SettingDef {
    const char* name;        // DValue name (must be <= 15 chars)
    SettingType type;
    int         default_val; // bool: 0/1, int: integer value
    int         current;     // loaded from .ini or set to default
    int         min_val;     // slider min (only meaningful for Int)
    int         max_val;     // slider max (only meaningful for Int)
};

// Master table of all AOReloaded settings. Add new settings here.
static SettingDef g_settings[] = {
    // name           type              default  current  min  max
    { "AOR_CamOn",    SettingType::Bool, 1,       1,       0,   1  },
    { "AOR_CYawSpd",  SettingType::Int,  2,       2,       1,  10  },
    { "AOR_MouseRun", SettingType::Bool, 1,       1,       0,   1  },
    { "AOR_AutoRun",  SettingType::Bool, 1,       1,       0,   1  },
    { "AOR_DebugLog", SettingType::Bool, 0,       0,       0,   1  },
    { "AOR_NumpadFix", SettingType::Bool, 1,       1,       0,   1  },
    { "AOR_TBarX",     SettingType::Int,  40,      40,      0, 4096 },
    { "AOR_TBarY",     SettingType::Int,  40,      40,      0, 4096 },
    { "AOR_TBarW",     SettingType::Int, 113,     113,     32, 1024 },
    { "AOR_TBarH",     SettingType::Int,  10,      10,      8,  128 },
    { "AOR_TBarPrev",  SettingType::Bool,  0,       0,      0,    1 },
    { "AOR_FpsCap",    SettingType::Int, 100,     100,     30,  500 },
    { "AOR_BagMove",   SettingType::Bool,  1,       1,      0,    1 },
    { "AOR_AltSelect", SettingType::Bool,  1,       1,      0,    1 },
    { "AOR_InvPins",   SettingType::Bool,  1,       1,      0,    1 },
    { "AOR_SkillFavs", SettingType::Bool,  1,       1,      0,    1 },
    { "AOR_SkillShift", SettingType::Bool, 1,       1,      0,    1 },
    // No widget of their own: the stock view distance sliders, past their
    // stock maximums (view_distance.cpp). 0 = never changed.
    { "AOR_CharDist",  SettingType::Int,   0,       0,      0,  300 },
    { "AOR_GroundHQ",  SettingType::Int,   0,       0,      0,  150 },
    { "AOR_ObjDist",   SettingType::Int, 100,     100,    100,  400 },
};

static constexpr int kSettingCount = sizeof(g_settings) / sizeof(g_settings[0]);

// ── Settings change callbacks ─────────────────────────────────────────

static constexpr int kMaxSettingCallbacks = 4;
static SettingChangedCallback g_settingCallbacks[kMaxSettingCallbacks] = {};
static int g_settingCallbackCount = 0;

void RegisterSettingCallback(SettingChangedCallback cb) {
    if (g_settingCallbackCount < kMaxSettingCallbacks)
        g_settingCallbacks[g_settingCallbackCount++] = cb;
}
static const char kIniSection[] = "AOReloaded";

// ── DValue tree min/max ────────────────────────────────────────────────
//
// The OptionSlider widget reads min/max from the DValue registry node to
// compute knob position: (value - min) / (max - min). Without min/max,
// the division is zero → crash. AddVariable doesn't set them, so we walk
// the tree and write directly.
//
// Node layout offsets (verified hex dump, see src/ao/CLAUDE.md):
//   +0x10: AOString key
//   +0x50: Variant (type tag + padding)
//   +0x58: value payload
//   +0x61: bool has_min
//   +0x62: bool has_max
//   +0x68: Variant min_value (0x10 bytes)
//   +0x78: Variant max_value (0x10 bytes)
//   +0xA1: char _Isnil (0=real, 1=sentinel)

// RVAs for the global std::map internals in Utils.dll.
static constexpr uint32_t kMyHeadRVA = 0x2e61c;

bool SetDValueMinMax(const char* name, int minVal, int maxVal) {
    HMODULE utils = GetModuleHandleA("Utils.dll");
    if (!utils) return false;

    auto base = reinterpret_cast<uint8_t*>(utils);

    // Read the tree's sentinel node (head).
    uint8_t* head = nullptr;
    __try {
        head = *reinterpret_cast<uint8_t**>(base + kMyHeadRVA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[settings] failed to read DValue tree head");
        return false;
    }
    if (!head) return false;

    // Walk the tree starting from head->_Left (the root).
    // In-order traversal looking for our key.
    constexpr int kKeyOffset   = 0x10;
    constexpr int kIsnilOffset = 0xA1;
    constexpr int kHasMinOffset = 0x61;
    constexpr int kHasMaxOffset = 0x62;
    constexpr int kMinValOffset = 0x68;
    constexpr int kMaxValOffset = 0x78;

    uint8_t* cur = nullptr;
    __try {
        // Start at leftmost (smallest key).
        cur = *reinterpret_cast<uint8_t**>(head);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    // Walk up to 500 nodes (safety bound — registry has ~270).
    for (int i = 0; i < 500; ++i) {
        __try {
            if (cur[kIsnilOffset] != 0) break;  // sentinel → done

            auto* key = reinterpret_cast<AOString*>(cur + kKeyOffset);
            const char* keyStr = key->c_str();
            if (keyStr && std::strcmp(keyStr, name) == 0) {
                // Found it. Write min/max.
                *reinterpret_cast<bool*>(cur + kHasMinOffset) = true;
                *reinterpret_cast<bool*>(cur + kHasMaxOffset) = true;
                auto* minVar = reinterpret_cast<AOVariant*>(cur + kMinValOffset);
                auto* maxVar = reinterpret_cast<AOVariant*>(cur + kMaxValOffset);
                *minVar = AOVariant::FromInt(minVal);
                *maxVar = AOVariant::FromInt(maxVal);
                Log("[settings] set min/max for %s: [%d, %d]", name, minVal, maxVal);
                return true;
            }

            // In-order successor: right subtree's leftmost, or walk up.
            uint8_t* right = *reinterpret_cast<uint8_t**>(cur + 0x08);
            if (right[kIsnilOffset] == 0) {
                cur = right;
                uint8_t* left = *reinterpret_cast<uint8_t**>(cur);
                while (left[kIsnilOffset] == 0) {
                    cur = left;
                    left = *reinterpret_cast<uint8_t**>(cur);
                }
            } else {
                uint8_t* par = *reinterpret_cast<uint8_t**>(cur + 0x04);
                while (par[kIsnilOffset] == 0 &&
                       cur == *reinterpret_cast<uint8_t**>(par + 0x08)) {
                    cur = par;
                    par = *reinterpret_cast<uint8_t**>(cur + 0x04);
                }
                cur = par;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[settings] AV while searching for %s", name);
            return false;
        }
    }

    Log("[settings] DValue %s not found in tree", name);
    return false;
}

// ── .ini file path ─────────────────────────────────────────────────────

static wchar_t g_iniPath[MAX_PATH] = {};
static bool    g_iniReady = false;

static void ResolveIniPath() {
    // The exe runs from the client directory. Place .ini alongside it,
    // same as AOReloaded.log.
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        Log("[settings] GetModuleFileName failed");
        return;
    }

    // Strip exe filename, keep trailing backslash.
    wchar_t* slash = nullptr;
    for (wchar_t* p = exePath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') slash = p;
    }
    if (!slash) return;
    *(slash + 1) = L'\0';

    if (swprintf(g_iniPath, MAX_PATH, L"%sAOReloaded.ini", exePath) < 0) {
        Log("[settings] path format failed");
        return;
    }

    g_iniReady = true;
    Log("[settings] ini path: %ls", g_iniPath);
}

// ── .ini read/write ────────────────────────────────────────────────────

static int ReadIniInt(const char* name, int defaultVal) {
    if (!g_iniReady) return defaultVal;
    // GetPrivateProfileStringA so we can use narrow-char section/key names
    // with the wide-char file path via GetPrivateProfileStringW would
    // require wide keys. Instead, use the A variant and convert the path.
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, g_iniPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);
    return GetPrivateProfileIntA(kIniSection, name, defaultVal, narrowPath);
}

static void WriteIniInt(const char* name, int value) {
    if (!g_iniReady) return;
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, g_iniPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", value);
    WritePrivateProfileStringA(kIniSection, name, buf, narrowPath);
}

// ── Find a setting by name ─────────────────────────────────────────────

static SettingDef* FindSetting(const char* name) {
    for (int i = 0; i < kSettingCount; ++i) {
        if (std::strcmp(g_settings[i].name, name) == 0)
            return &g_settings[i];
    }
    return nullptr;
}

int SettingsGetInt(const char* name) {
    const SettingDef* def = FindSetting(name);
    return def ? def->current : 0;
}

void SettingsSetInt(const char* name, int value) {
    SettingDef* def = FindSetting(name);
    if (!def || def->current == value) return;
    def->current = value;
    WriteIniInt(def->name, value);
    Log("[settings] persisted %s = %d", def->name, value);
}

// ── SetDValue hook ─────────────────────────────────────────────────────
//
// Detours the game's static SetDValue to intercept writes to our settings.
// After calling the original, checks if the name matches any AOReloaded
// setting and persists the new value to the .ini.

static GameAPI::FnSetDValue g_origSetDValue = nullptr;

static void __cdecl SetDValueDetour(const AOString& name, const AOVariant& value) {
    // Always call the original first — the game must see the change.
    g_origSetDValue(name, value);

    // Quick prefix check: all our settings start with "AOR_"; "RVK_" ones belong to the renderer, which
    // applies and saves them itself.
    const char* str = name.c_str();
    if (RendererOnSetDValue(str, value))
        return;
    ViewDistanceOnSetDValue(str, value);
    if (str[0] != 'A' || str[1] != 'O' || str[2] != 'R' || str[3] != '_')
        return;

    SettingDef* def = FindSetting(str);
    if (!def) return;

    // Extract the value based on the setting's type.
    // The OptionSlider widget sends Double variants (Slider_c uses double
    // internally), so we must handle Double in addition to Int/Float.
    int intVal = def->current;
    switch (def->type) {
    case SettingType::Bool:
        if (value.type == static_cast<uint32_t>(VariantType::Bool))
            intVal = value.as_bool ? 1 : 0;
        else if (value.type == static_cast<uint32_t>(VariantType::Int))
            intVal = value.as_int ? 1 : 0;
        else if (value.type == static_cast<uint32_t>(VariantType::Double))
            intVal = value.as_double != 0.0 ? 1 : 0;
        break;
    case SettingType::Int:
        if (value.type == static_cast<uint32_t>(VariantType::Int))
            intVal = value.as_int;
        else if (value.type == static_cast<uint32_t>(VariantType::Float))
            intVal = static_cast<int>(value.as_float);
        else if (value.type == static_cast<uint32_t>(VariantType::Double))
            intVal = static_cast<int>(value.as_double);
        break;
    }

    if (intVal != def->current) {
        def->current = intVal;
        WriteIniInt(def->name, intVal);
        Log("[settings] persisted %s = %d", def->name, intVal);

        // Notify registered listeners.
        for (int i = 0; i < g_settingCallbackCount; ++i) {
            if (g_settingCallbacks[i])
                g_settingCallbacks[i](def->name, intVal);
        }
    }
}

// ── .ini path helper (standalone, no Log dependency) ───────────────────
//
// Builds the absolute path to AOReloaded.ini next to the exe. Used by
// IsDebugLogEnabled (called before logging is initialized) and by the
// normal settings path resolver.

static bool BuildIniPathNarrow(char* out, int outSize) {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    wchar_t* slash = nullptr;
    for (wchar_t* p = exePath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') slash = p;
    }
    if (!slash) return false;
    *(slash + 1) = L'\0';

    wchar_t widePath[MAX_PATH];
    if (swprintf(widePath, MAX_PATH, L"%sAOReloaded.ini", exePath) < 0)
        return false;

    WideCharToMultiByte(CP_ACP, 0, widePath, -1, out, outSize, nullptr, nullptr);
    return true;
}

// ── Options panel XML injection ────────────────────────────────────────
//
// The AOReloaded tab in Root.xml defines the in-game settings UI. Rather
// than requiring users to manually edit the file, we inject it at runtime
// if it's missing. The XML block is a static string kept here alongside
// the settings table so new settings only need to be added in one place
// (the table for persistence, and the XML for UI).

static const char kAorXmlHead[] =
    "\n"
    "  <ScrollView h_alignment=\"LEFT\" label=\"AOReloaded\" v_scrollbar_mode=\"auto\""
    " scroll_client=\"aor_scroll\" max_size=\"Point(16000,-1)\">\n"
    "    <ScrollViewChild name=\"aor_scroll\">\n"
    "      <View h_alignment=\"LEFT\" view_layout=\"vertical\""
    " max_size=\"Point(16000,-1)\" layout_borders=\"Rect(0,0,10,0)\">\n"
    "        <TextView value=\"AOReloaded v0.1.0\" layout_borders=\"Rect(0,0,0,5)\" />\n"
    "        <TextView value=\"Client mod framework. Settings are saved to AOReloaded.ini.\""
    " layout_borders=\"Rect(0,0,0,10)\" />\n"
    "\n"
    "        <TextView value=\"Camera\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <OptionCheckBox label=\"WoW-style camera (auto-recenter after LMB drag)\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_CamOn\"/>\n"
    "        <OptionSlider label=\"Camera recenter speed:\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_CYawSpd\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionCheckBox label=\"LMB+RMB mouse-run (hold both mouse buttons to run forward - requires WoW camera enabled)\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_MouseRun\"/>\n"
    "        <OptionCheckBox label=\"Toggle autorun &amp; work smoothly with forward-key\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_AutoRun\"/>\n"
    "\n"
    "        <TextView value=\"Timer Bars\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <TextView value=\"Cast/action timer bar position. Drag any active bar in-game, or use these sliders. Default: 40, 40.\""
    " layout_borders=\"Rect(10,0,0,3)\" />\n"
    "        <OptionSlider label=\"X position:\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_TBarX\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionSlider label=\"Y position:\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_TBarY\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionSlider label=\"Width (default: 113):\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_TBarW\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionSlider label=\"Height (default: 10):\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_TBarH\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionCheckBox label=\"Enable cast bar preview & drag (stack extends downwards from the topmost bar)\""
    " layout_borders=\"Rect(10,5,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_TBarPrev\"/>\n"
    "\n"
    "        <TextView value=\"View distance\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <OptionSlider label=\"Object detail distance, % of the stock distances (applies from the next zone):\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_ObjDist\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f%%&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "\n"
    "        <TextView value=\"Input\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <OptionCheckBox label=\"Numpad keys type in chat fix\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_NumpadFix\"/>\n"
    "        <OptionCheckBox label=\"Ctrl+click moves items between the inventory and the last opened backpack\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_BagMove\"/>\n"
    "        <OptionCheckBox label=\"Alt+click selects several items; moving one moves them all\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_AltSelect\"/>\n"
    "        <OptionCheckBox label=\"Ctrl+Alt+click pins an item to the top of the inventory list view\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_InvPins\"/>\n"
    "        <OptionCheckBox label=\"Ctrl+Alt+click a skill to add it to the Favorites group of the skills window\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_SkillFavs\"/>\n"
    "        <OptionCheckBox label=\"Shift+click on a skill's + or - changes it by 5\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_SkillShift\"/>\n"
    "\n";

// The Performance section: only without a Renderer tab, which shows the
// frame rate slider first instead (renderer_settings.cpp).
static const char kAorXmlPerformance[] =
    "        <TextView value=\"Performance\" layout_borders=\"Rect(0,10,0,3)\" />\n";

static const char kAorXmlTail[] =
    "\n"
    "        <TextView value=\"Debug\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <OptionCheckBox label=\"Enable debug logging (AOReloaded.log, requires restart)\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_DebugLog\"/>\n"
    "\n"
    "        <VLayoutSpacer/>\n"
    "      </View>\n"
    "    </ScrollViewChild>\n"
    "  </ScrollView>\n";

// ── Root.xml overlay ───────────────────────────────────────────────────
//
// Custom GUIs in AO live in:
//   %LocalAppData%\Funcom\Anarchy Online\<hash>\<sub>\Gui\<GUIName>\
// The game loads cd_image/gui/Default/ first, then overlays files from
// the custom GUI directory. If the custom GUI has its own
// OptionPanel/Root.xml, that one takes precedence.
//
// The AOReloaded/Renderer tabs are injected in memory by the overlay
// (gui_overlay.cpp), which matches whichever OptionPanel/Root.xml the
// client actually opens — no need to guess the active GUI. Older builds
// wrote the blocks into every Root.xml they could find; CleanRootXmlOnDisk
// (via ForEachGuiFile) strips those, skipping non-existent paths.

// Get the client directory (exe dir with trailing backslash).
static bool GetClientDir(char* out, int outSize) {
    if (!BuildIniPathNarrow(out, outSize)) return false;
    char* lastSlash = nullptr;
    for (char* p = out; *p; ++p) {
        if (*p == '\\' || *p == '/') lastSlash = p;
    }
    if (!lastSlash) return false;
    *(lastSlash + 1) = '\0';
    return true;
}

// The AOReloaded tab's XML: its sections, with Performance (the frame rate
// slider) only when there's no Renderer tab to show it.
static const char* AorXmlBlock() {
    constexpr size_t kSliderRoom = 512;         // kFpsCapSliderXml (fps_cap.cpp) is ~250 bytes
    static char block[sizeof(kAorXmlHead) + sizeof(kAorXmlPerformance) + kSliderRoom + sizeof(kAorXmlTail)];
    block[0] = '\0';
    std::strcat(block, kAorXmlHead);
    if (!RendererXmlBlock() && std::strlen(kFpsCapSliderXml) < kSliderRoom) {
        std::strcat(block, kAorXmlPerformance);
        std::strcat(block, kFpsCapSliderXml);
    }
    std::strcat(block, kAorXmlTail);
    return block;
}

// Remove any AOReloaded/Renderer <ScrollView> blocks. Used both to make the
// in-memory injection idempotent and to repair files older builds edited on
// disk. Returns true if anything was removed.
static bool StripRootBlocks(std::string& xml) {
    bool removed = false;
    size_t search = 0;
    while (true) {
        const size_t start = xml.find("<ScrollView", search);
        if (start == std::string::npos) break;

        const size_t tagEnd = xml.find('>', start);
        const size_t aor = xml.find("label=\"AOReloaded\"", start);
        const size_t rvk = xml.find("label=\"Renderer\"", start);
        const bool isOurs =
            tagEnd != std::string::npos &&
            ((aor != std::string::npos && aor < tagEnd + 1) ||
             (rvk != std::string::npos && rvk < tagEnd + 1));
        if (!isOurs) {
            search = start + 1;
            continue;
        }

        size_t close = xml.find("</ScrollView>", start);
        if (close == std::string::npos) break;
        close += 13;  // skip "</ScrollView>"
        while (close < xml.size() && (xml[close] == '\n' || xml[close] == '\r')) ++close;
        xml.erase(start, close - start);
        removed = true;
        search = start;  // the other block may directly follow
    }
    return removed;
}

// Add the current AOReloaded tab (and the Renderer tab, when a renderer with
// a settings interface is loaded) before </root>.
static void InjectRootBlocks(std::string& xml) {
    const size_t end = xml.rfind("</root>");
    if (end == std::string::npos) return;
    std::string blocks(AorXmlBlock());
    if (const char* rendererBlock = RendererXmlBlock()) blocks += rendererBlock;
    xml.insert(end, blocks);
}

// Overlay patcher: strip stale blocks and add the current ones. Returns false
// (file served unchanged) when there is nothing to change, which is also the
// fast path once the on-disk files have been repaired.
static bool RootXmlPatcher(const std::string& in, std::string& out) {
    std::string work = in;
    StripRootBlocks(work);
    InjectRootBlocks(work);
    if (work == in) return false;
    out.swap(work);
    return true;
}

void RegisterRootXmlOverlay() {
    overlay::RegisterPatcher("OptionPanel/Root.xml", &RootXmlPatcher);
}

// One-time repair: older builds wrote the blocks into the file. Strip them so
// that deleting version.dll really restores the stock GUI.
static void CleanRootXmlFile(const char* xmlPath) {
    HANDLE hFile = CreateFileA(xmlPath, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    const DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize > 1024 * 1024) {
        CloseHandle(hFile);
        return;
    }

    std::string xml;
    xml.resize(fileSize);
    DWORD bytesRead = 0;
    const bool read = fileSize == 0 ||
        (ReadFile(hFile, &xml[0], fileSize, &bytesRead, nullptr) && bytesRead == fileSize);
    CloseHandle(hFile);
    if (!read || !StripRootBlocks(xml)) return;

    hFile = CreateFileA(xmlPath, GENERIC_WRITE, 0,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log("[settings] cannot restore %s (%lu)", xmlPath, GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(hFile, xml.data(), static_cast<DWORD>(xml.size()), &written, nullptr);
    CloseHandle(hFile);
    Log("[settings] restored stock Root.xml: %s", xmlPath);
}

void CleanRootXmlOnDisk() {
    ForEachGuiFile("OptionPanel\\Root.xml", CleanRootXmlFile);
}

// ── Public API ─────────────────────────────────────────────────────────

bool IsDebugLogEnabled() {
    char iniPath[MAX_PATH];
    if (!BuildIniPathNarrow(iniPath, MAX_PATH)) return false;
    return GetPrivateProfileIntA(kIniSection, "AOR_DebugLog", 0, iniPath) != 0;
}

void ForEachGuiFile(const char* relPath, void (*fn)(const char* path)) {
    char clientDir[MAX_PATH];
    if (!GetClientDir(clientDir, MAX_PATH)) {
        Log("[settings] could not resolve client directory");
        return;
    }

    // 1. Always patch Default in cd_image.
    {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    "%scd_image\\gui\\Default\\%s", clientDir, relPath);
        fn(path);
    }

    // 2. Scan every GUI in cd_image/gui/ (covers non-Default GUIs shipped
    //    alongside the client).
    {
        char searchPattern[MAX_PATH];
        _snprintf_s(searchPattern, sizeof(searchPattern), _TRUNCATE,
                    "%scd_image\\gui\\*", clientDir);

        WIN32_FIND_DATAA entry;
        HANDLE hFind = FindFirstFileA(searchPattern, &entry);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (entry.cFileName[0] == '.') continue;
                if (_stricmp(entry.cFileName, "Default") == 0) continue;  // already done

                char path[MAX_PATH];
                _snprintf_s(path, sizeof(path), _TRUNCATE,
                            "%scd_image\\gui\\%s\\%s",
                            clientDir, entry.cFileName, relPath);
                fn(path);
            } while (FindNextFileA(hFind, &entry));
            FindClose(hFind);
        }
    }

    // 3. Scan %LocalAppData%\Funcom\Anarchy Online\<hash>\<sub>\Gui\*\
    //    for custom GUIs installed via the standard AO user-install method.
    //    Structure: <hash>\<sub>\Gui\<GUIName>\<relPath>
    {
        char appDataBase[MAX_PATH];
        DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", appDataBase, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) goto done_appdata;

        char hashPattern[MAX_PATH];
        _snprintf_s(hashPattern, sizeof(hashPattern), _TRUNCATE,
                    "%s\\Funcom\\Anarchy Online\\*", appDataBase);

        WIN32_FIND_DATAA hashEntry;
        HANDLE hHash = FindFirstFileA(hashPattern, &hashEntry);
        if (hHash == INVALID_HANDLE_VALUE) goto done_appdata;

        do {
            if (!(hashEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (hashEntry.cFileName[0] == '.') continue;

            // Enumerate subdirectories (e.g. "Anarchy Online" or "client").
            char subPattern[MAX_PATH];
            _snprintf_s(subPattern, sizeof(subPattern), _TRUNCATE,
                        "%s\\Funcom\\Anarchy Online\\%s\\*",
                        appDataBase, hashEntry.cFileName);

            WIN32_FIND_DATAA subEntry;
            HANDLE hSub = FindFirstFileA(subPattern, &subEntry);
            if (hSub == INVALID_HANDLE_VALUE) continue;

            do {
                if (!(subEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (subEntry.cFileName[0] == '.') continue;

                // Enumerate GUI names inside this Gui folder.
                char guiPattern[MAX_PATH];
                _snprintf_s(guiPattern, sizeof(guiPattern), _TRUNCATE,
                            "%s\\Funcom\\Anarchy Online\\%s\\%s\\Gui\\*",
                            appDataBase, hashEntry.cFileName, subEntry.cFileName);

                WIN32_FIND_DATAA guiEntry;
                HANDLE hGui = FindFirstFileA(guiPattern, &guiEntry);
                if (hGui == INVALID_HANDLE_VALUE) continue;

                do {
                    if (!(guiEntry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (guiEntry.cFileName[0] == '.') continue;

                    char path[MAX_PATH];
                    _snprintf_s(path, sizeof(path), _TRUNCATE,
                                "%s\\Funcom\\Anarchy Online\\%s\\%s"
                                "\\Gui\\%s\\%s",
                                appDataBase, hashEntry.cFileName,
                                subEntry.cFileName, guiEntry.cFileName, relPath);
                    fn(path);
                } while (FindNextFileA(hGui, &guiEntry));
                FindClose(hGui);
            } while (FindNextFileA(hSub, &subEntry));
            FindClose(hSub);
        } while (FindNextFileA(hHash, &hashEntry));
        FindClose(hHash);
    }
done_appdata:;
}

void SettingsInit() {
    ResolveIniPath();

    // Load persisted values from .ini (or use defaults), then write back
    // so the .ini always contains the full set of settings. This ensures
    // new settings added in updates appear in the file immediately with
    // their defaults, and the .ini is self-documenting for hand-editing.
    for (int i = 0; i < kSettingCount; ++i) {
        g_settings[i].current = ReadIniInt(
            g_settings[i].name, g_settings[i].default_val);
        WriteIniInt(g_settings[i].name, g_settings[i].current);
        Log("[settings] loaded %s = %d (default %d)",
            g_settings[i].name, g_settings[i].current, g_settings[i].default_val);
    }
}

void SettingsRegisterAll() {
    for (int i = 0; i < kSettingCount; ++i) {
        const SettingDef& s = g_settings[i];
        switch (s.type) {
        case SettingType::Bool:
            GameAPI::RegisterBool(s.name, s.current != 0);
            break;
        case SettingType::Int:
            GameAPI::RegisterInt(s.name, s.current);
            // Sliders require min/max in the DValue registry node,
            // otherwise the knob doesn't render and clicking crashes.
            SetDValueMinMax(s.name, s.min_val, s.max_val);
            break;
        }
    }
    // A custom renderer's settings (randy-vk), if it has a settings interface.
    RendererSettingsRegisterAll();
}

bool SettingsInstallHook() {
    // SetDValue was already resolved by GameAPI::Init(). Hook it.
    void* target = reinterpret_cast<void*>(GameAPI::SetDValue);
    if (!target) {
        Log("[settings] SetDValue not resolved — hook skipped");
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(target);
    Log("[settings] SetDValue at %p, prologue: %02X %02X %02X %02X %02X",
        target, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* trampoline = nullptr;
    if (!InstallHook(target, reinterpret_cast<void*>(&SetDValueDetour), &trampoline)) {
        Log("[settings] SetDValue hook failed — changes won't persist to .ini");
        return false;
    }

    g_origSetDValue = reinterpret_cast<GameAPI::FnSetDValue>(trampoline);

    // Also update the GameAPI function pointer so other code that calls
    // GameAPI::SetDValue goes through the trampoline (not the now-patched
    // address which would infinite-loop).
    GameAPI::SetDValue = g_origSetDValue;

    Log("[settings] SetDValue hook installed — settings will persist to .ini");
    return true;
}

}  // namespace aor
