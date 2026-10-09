// view_distance.cpp — Characters and full-quality ground further out.
//
// How the client limits them:
//   - SetDValue (Utils.dll) clamps every value to the min/max of its DValue
//     registry node; LoginPrefs.xml gives DisplayCharViewDistance 5..80 and
//     DisplayGroundFullQualityRadius 7..44.
//   - N3.dll FUN_1001f964 copies DisplayCharViewDistance into n3Camera_t
//     +0x174, replacing anything below 5 or above 80 with 70. On ground
//     playfields n3VisualDynel_t::Run hides every character further than that
//     from the player (a hard cut, no fade).
//   - DisplayGroundFullQualityRadius goes unchanged to
//     AnarchyGround_t::SetHQRadius (n3GroundRenderer_t's slot): ground
//     triangles within it get their own high-resolution texture, the rest
//     share the low-resolution one - the visible ring in the distance.
//
// So: raise both registry maximums, jump over N3's upper clamp, and keep the
// chosen values in AOReloaded.ini. The client loads its prefs (and clamps
// them to the stock maximums) before we can raise those, so the values are
// set again once, on the game thread, after login.

#include "hooks/view_distance.h"
#include "core/settings.h"
#include "core/logging.h"
#include "ao/game_api.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace aor {

namespace {

constexpr char kCharVar[]   = "DisplayCharViewDistance";
constexpr char kGroundVar[] = "DisplayGroundFullQualityRadius";
constexpr char kCharSetting[]   = "AOR_CharDist";
constexpr char kGroundSetting[] = "AOR_GroundHQ";

// Stock minimums (LoginPrefs.xml); maximums raised from 80 and 44.
constexpr int kCharMin = 5, kCharMax = 300;
constexpr int kGroundMin = 7, kGroundMax = 150;

// N3.dll FUN_1001f964:
//   1001f9a6  83 7D F0 50   cmp dword ptr [ebp-0x10], 0x50
//   1001f9aa  7E 07         jle 1001f9b3          ; the fild that stores it
//   1001f9ac  C7 45 F0 46.. mov dword ptr [ebp-0x10], 0x46
// becomes a jmp to 1001f9b3, so only the lower clamp (5) is left.
constexpr uint32_t kClampRva = 0x1f9a6;
constexpr uint8_t kClampOriginal[] = {0x83, 0x7D, 0xF0, 0x50, 0x7E, 0x07};
constexpr uint8_t kClampPatched[]  = {0xEB, 0x0B, 0x90, 0x90, 0x90, 0x90};

constexpr uint32_t kCameraCharDistance = 0x174;   // n3Camera_t, float metres

using FnGetEngineInstance = void*(__cdecl*)();
using FnGetActiveCamera   = void*(__thiscall*)(void* engine);
FnGetEngineInstance g_getEngine = nullptr;
FnGetActiveCamera   g_getActiveCamera = nullptr;

bool g_clampPatched = false;
volatile bool g_reapplyPending = false;

// A heap-mode AOString over a static name (> 15 chars); SetDValue and
// GetDValue only read it.
AOString LongName(const char* str) {
    AOString s;
    std::memset(&s, 0, sizeof(s));
    s.heap_ptr = const_cast<char*>(str);
    s.length = static_cast<uint32_t>(std::strlen(str));
    s.capacity = s.length;
    return s;
}

bool AsInt(const AOVariant& v, int& out) {
    switch (static_cast<VariantType>(v.type)) {
    case VariantType::Int:    out = v.as_int; return true;
    case VariantType::Float:  out = static_cast<int>(v.as_float); return true;
    case VariantType::Double: out = static_cast<int>(v.as_double); return true;
    default: return false;
    }
}

bool GetInt(const char* name, int& out) {
    if (!GameAPI::GetDValue) return false;
    AOVariant v{};
    GameAPI::GetDValue(&v, LongName(name), false);
    return AsInt(v, out);
}

// Set a numeric DValue, keeping the type it has (the sliders store doubles).
void SetInt(const char* name, int value) {
    AOVariant current{};
    GameAPI::GetDValue(&current, LongName(name), false);
    AOVariant v = AOVariant::FromInt(value);
    if (current.type == static_cast<uint32_t>(VariantType::Float)) v = AOVariant::FromFloat(static_cast<float>(value));
    else if (current.type == static_cast<uint32_t>(VariantType::Double)) v = AOVariant::FromDouble(value);
    GameAPI::SetDValue(LongName(name), v);
}

// The camera reads the setting only when it is built; set the active one too.
void SetCameraCharDistance(int metres) {
    if (!g_getEngine || !g_getActiveCamera) return;
    void* engine = g_getEngine();
    void* camera = engine ? g_getActiveCamera(engine) : nullptr;
    if (!camera) return;
    *reinterpret_cast<float*>(static_cast<uint8_t*>(camera) + kCameraCharDistance) = static_cast<float>(metres);
}

bool PatchClamp(HMODULE n3) {
    auto* at = reinterpret_cast<uint8_t*>(n3) + kClampRva;
    if (std::memcmp(at, kClampPatched, sizeof(kClampPatched)) == 0) return true;
    if (std::memcmp(at, kClampOriginal, sizeof(kClampOriginal)) != 0) {
        Log("[viewdist] N3 clamp bytes differ (%02X %02X %02X %02X) - not patched, characters stay at <= 80 m",
            at[0], at[1], at[2], at[3]);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(kClampPatched), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, kClampPatched, sizeof(kClampPatched));
    VirtualProtect(at, sizeof(kClampPatched), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kClampPatched));
    return true;
}

// Put a remembered value back (within the raised slider range).
void Reapply(const char* var, const char* setting, int lo, int hi) {
    int wanted = SettingsGetInt(setting);
    if (wanted <= 0) return;                        // never changed: the client's own value stands
    if (wanted < lo) wanted = lo;
    if (wanted > hi) wanted = hi;
    int current = 0;
    if (GetInt(var, current) && current == wanted) return;
    SetInt(var, wanted);
    Log("[viewdist] %s %d -> %d", var, current, wanted);
}

}  // namespace

bool PatchViewDistanceClamp() {
    HMODULE n3 = GetModuleHandleA("N3.dll");
    if (!n3) {
        Log("[viewdist] N3.dll not loaded");
        return false;
    }
    g_getEngine = reinterpret_cast<FnGetEngineInstance>(GetProcAddress(n3, "?GetInstance@n3EngineClient_t@@SAPAV1@XZ"));
    g_getActiveCamera = reinterpret_cast<FnGetActiveCamera>(
        GetProcAddress(n3, "?GetActiveCamera@n3EngineClient_t@@QBEPAVn3Camera_t@@XZ"));
    g_clampPatched = PatchClamp(n3);
    return g_clampPatched;
}

bool InitViewDistance() {
    const bool charRange = g_clampPatched && SetDValueMinMax(kCharVar, kCharMin, kCharMax);
    const bool groundRange = SetDValueMinMax(kGroundVar, kGroundMin, kGroundMax);
    Log("[viewdist] character distance up to %d m: %s, ground full quality up to %d: %s", kCharMax,
        charRange ? "yes" : "no", kGroundMax, groundRange ? "yes" : "no");
    g_reapplyPending = true;
    return charRange || groundRange;
}

void ViewDistanceTick() {
    if (!g_reapplyPending) return;
    g_reapplyPending = false;
    Reapply(kCharVar, kCharSetting, kCharMin, kCharMax);
    Reapply(kGroundVar, kGroundSetting, kGroundMin, kGroundMax);
    int metres = 0;
    if (GetInt(kCharVar, metres)) SetCameraCharDistance(metres);
}

void ViewDistanceOnSetDValue(const char* name, const AOVariant& value) {
    int v = 0;
    if (std::strcmp(name, kCharVar) == 0 && AsInt(value, v)) {
        SettingsSetInt(kCharSetting, v);
        if (GetInt(kCharVar, v)) SetCameraCharDistance(v);   // what SetDValue kept after clamping
    } else if (std::strcmp(name, kGroundVar) == 0 && AsInt(value, v)) {
        SettingsSetInt(kGroundSetting, v);
    }
}

}  // namespace aor
