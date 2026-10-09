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
//   - Past 44 the ground corrupts the heap (crash, 2026-10-09): see
//     PatchGroundIndexOffsets.
//   - Outdoors, placed objects (statels: buildings' props, lamps, crates...)
//     come and go with distance in N3's statel grid: see PatchStatelRings.
//
// So: raise both registry maximums, jump over N3's upper clamp, fix the
// ground's index offsets, and keep the
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
constexpr char kObjSetting[]    = "AOR_ObjDist";

// Stock minimums (LoginPrefs.xml); maximums raised from 80 and 44.
constexpr int kCharMin = 5, kCharMax = 300;
constexpr int kGroundMin = 7, kGroundMax = 150, kGroundStockMax = 44;

// N3.dll FUN_1001f964:
//   1001f9a6  83 7D F0 50   cmp dword ptr [ebp-0x10], 0x50
//   1001f9aa  7E 07         jle 1001f9b3          ; the fild that stores it
//   1001f9ac  C7 45 F0 46.. mov dword ptr [ebp-0x10], 0x46
// becomes a jmp to 1001f9b3, so only the lower clamp (5) is left.
constexpr uint32_t kClampRva = 0x1f9a6;
constexpr uint8_t kClampOriginal[] = {0x83, 0x7D, 0xF0, 0x50, 0x7E, 0x07};
constexpr uint8_t kClampPatched[]  = {0xEB, 0x0B, 0x90, 0x90, 0x90, 0x90};

// DisplaySystem.dll FUN_10034658 (AnarchyGround_t, laying the tessellated
// ground out into pooled vertex buffers): each high-quality texture group gets
// a range of its buffer's index array (allocated once, (capacity + 1) * 3 / 2
// entries, FUN_100384b3) at an offset taken from [ebp-0x40]. That counter runs
// on over every group, never going back to 0 when the layout moves on to the
// next buffer, so later buffers are written past the end of their arrays once
// there are enough high-quality triangles - never at the stock radius of 44.
// The draw (FUN_100385b3) reads each group's indices from its own buffer's
// array at that offset, so restarting the counter for every buffer is right.
// Every move to the next buffer goes through 100347c5 (the only jump into it):
//   100347c5  2B 8E AC 05 00 00   sub ecx, [esi+0x5ac]
// becomes a jmp to a stub doing "and dword ptr [ebp-0x40], 0" first.
constexpr uint32_t kNextBufferRva = 0x347c5;
constexpr uint8_t kNextBufferOriginal[] = {0x2B, 0x8E, 0xAC, 0x05, 0x00, 0x00};

// N3.dll statel grid (built per playfield, FUN_10028ebd): the
// playfield is split into cells, each holding its statels in six size
// classes. FUN_10028ab7 puts a cell in a ring by the distance from the camera
// to its centre, against 0.5 x the camera's view cone length (ViewDistance x
// 1000, so ~500 m) x five factors (min 40 m each): 0.1, 0.15, 0.3, 0.4, 0.55
// -> 50 / 75 / 150 / 200 / 275 m. FUN_100286a9 shows fewer of the classes the
// further out the ring (small things only within 50-75 m) - the pop-in the
// pop-in log found at 50-100 m in Newland City. The grid copies the factors
// into +0x2c..+0x3c from shared constants (also used elsewhere), so the five
// fld [constant] there are pointed at our own, scaled factors instead. Read
// when a playfield's grid is built: changes apply from the next zone.
constexpr uint32_t kRingLoadRvas[5] = {0x28f92, 0x28f9b, 0x28fa4, 0x28fad, 0x28fb6};
constexpr uint32_t kRingConstRvas[5] = {0x3d61c, 0x3e674, 0x3e29c, 0x3e670, 0x3e66c};
constexpr float kRingFactors[5] = {0.1f, 0.15f, 0.3f, 0.4f, 0.55f};
constexpr int kObjMin = 100, kObjMax = 400;      // AOR_ObjDist, percent of the stock rings
float g_rings[5] = {0.1f, 0.15f, 0.3f, 0.4f, 0.55f};

constexpr uint32_t kCameraCharDistance = 0x174;   // n3Camera_t, float metres

using FnGetEngineInstance = void*(__cdecl*)();
using FnGetActiveCamera   = void*(__thiscall*)(void* engine);
FnGetEngineInstance g_getEngine = nullptr;
FnGetActiveCamera   g_getActiveCamera = nullptr;

bool g_clampPatched = false;
bool g_groundPatched = false;
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

bool PatchGroundIndexOffsets(HMODULE ds) {
    auto* at = reinterpret_cast<uint8_t*>(ds) + kNextBufferRva;
    if (at[0] == 0xE9) return true;                  // already ours
    if (std::memcmp(at, kNextBufferOriginal, sizeof(kNextBufferOriginal)) != 0) {
        Log("[viewdist] DisplaySystem ground layout bytes differ (%02X %02X %02X) - not patched, ground stays at <= 44",
            at[0], at[1], at[2]);
        return false;
    }
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    const uint8_t* back = at + sizeof(kNextBufferOriginal);
    uint8_t code[] = {
        0x83, 0x65, 0xC0, 0x00,                      // and dword ptr [ebp-0x40], 0
        0x2B, 0x8E, 0xAC, 0x05, 0x00, 0x00,          // sub ecx, [esi+0x5ac]   (the original)
        0xE9, 0, 0, 0, 0,                            // jmp 100347cb
    };
    const int32_t backRel = static_cast<int32_t>(back - (stub + sizeof(code)));
    std::memcpy(code + 11, &backRel, 4);
    std::memcpy(stub, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(code));

    uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};      // jmp stub; nop
    const int32_t rel = static_cast<int32_t>(stub - (at + 5));
    std::memcpy(jump + 1, &rel, 4);
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, jump, sizeof(jump));
    VirtualProtect(at, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(jump));
    return true;
}

bool PatchStatelRings(HMODULE n3) {
    auto* base = reinterpret_cast<uint8_t*>(n3);
    for (int i = 0; i < 5; ++i) {                    // check all five before touching any
        const uint8_t* at = base + kRingLoadRvas[i];
        const uint32_t constant = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base) + kRingConstRvas[i]);
        const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_rings[i]));
        uint32_t operand;
        std::memcpy(&operand, at + 2, 4);
        if (at[0] != 0xD9 || at[1] != 0x05 || (operand != constant && operand != ours)) {
            Log("[viewdist] N3 statel ring loads differ (%02X %02X %08X) - not patched, objects keep the stock distances",
                at[0], at[1], operand);
            return false;
        }
    }
    for (int i = 0; i < 5; ++i) {
        uint8_t* at = base + kRingLoadRvas[i] + 2;
        const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_rings[i]));
        DWORD old = 0;
        if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(at, &ours, 4);
        VirtualProtect(at, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), base + kRingLoadRvas[0], kRingLoadRvas[4] + 6 - kRingLoadRvas[0]);
    return true;
}

void SetStatelRings(int percent) {
    if (percent < kObjMin) percent = kObjMin;
    if (percent > kObjMax) percent = kObjMax;
    for (int i = 0; i < 5; ++i) g_rings[i] = kRingFactors[i] * static_cast<float>(percent) / 100.0f;
    Log("[viewdist] statel rings at %d%%: %.0f / %.0f / %.0f / %.0f / %.0f m (from the next zone)", percent,
        g_rings[0] * 500.0f, g_rings[1] * 500.0f, g_rings[2] * 500.0f, g_rings[3] * 500.0f, g_rings[4] * 500.0f);
}

void OnSettingChanged(const char* name, int value) {
    if (std::strcmp(name, kObjSetting) == 0) SetStatelRings(value);
}

int GroundMax() { return g_groundPatched ? kGroundMax : kGroundStockMax; }

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

bool PatchViewDistanceCode() {
    HMODULE ds = GetModuleHandleA("DisplaySystem.dll");
    g_groundPatched = ds && PatchGroundIndexOffsets(ds);
    HMODULE n3 = GetModuleHandleA("N3.dll");
    if (!n3) {
        Log("[viewdist] N3.dll not loaded");
        return g_groundPatched;
    }
    g_getEngine = reinterpret_cast<FnGetEngineInstance>(GetProcAddress(n3, "?GetInstance@n3EngineClient_t@@SAPAV1@XZ"));
    g_getActiveCamera = reinterpret_cast<FnGetActiveCamera>(
        GetProcAddress(n3, "?GetActiveCamera@n3EngineClient_t@@QBEPAVn3Camera_t@@XZ"));
    g_clampPatched = PatchClamp(n3);
    SetStatelRings(SettingsGetInt(kObjSetting));   // the .ini is read already; the grid isn't built yet
    const bool rings = PatchStatelRings(n3);
    RegisterSettingCallback(&OnSettingChanged);
    return g_clampPatched || g_groundPatched || rings;
}

bool InitViewDistance() {
    const bool charRange = g_clampPatched && SetDValueMinMax(kCharVar, kCharMin, kCharMax);
    const bool groundRange = g_groundPatched && SetDValueMinMax(kGroundVar, kGroundMin, kGroundMax);
    Log("[viewdist] character distance up to %d m: %s, ground full quality up to %d: %s", kCharMax,
        charRange ? "yes" : "no", kGroundMax, groundRange ? "yes" : "no");
    g_reapplyPending = true;
    return charRange || groundRange;
}

void ViewDistanceTick() {
    if (!g_reapplyPending) return;
    g_reapplyPending = false;
    Reapply(kCharVar, kCharSetting, kCharMin, kCharMax);
    Reapply(kGroundVar, kGroundSetting, kGroundMin, GroundMax());
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
