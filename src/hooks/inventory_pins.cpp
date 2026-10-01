// inventory_pins.cpp — pinned items stay at the top of sorted inventory lists.
//
// See inventory_pins.h for the design overview and
// docs/inventory_trade_clicks.md for the sort internals this relies on.

#include "hooks/inventory_pins.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "ao/types.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace aor {

struct AOIdentity {
    uint32_t type;
    uint32_t instance;
};
static_assert(sizeof(AOIdentity) == 8, "Identity_t is 8 bytes");

static bool SameIdentity(const AOIdentity& a, const AOIdentity& b) {
    return a.type == b.type && a.instance == b.instance;
}

// ── Game functions ─────────────────────────────────────────────────────

// InventoryListViewItem_c primary vtable (GUI.dll .rdata, not exported).
// Slot 1 is Compare(MultiListViewItem_c const* other, int column), __thiscall,
// RET 8: column 0 = grid position, 1 = name, 2..16 = ints at +0x70+col*4.
// Ascending order puts `this` first when it returns < 0, descending when > 0.
constexpr uint32_t kItemVtableRVA  = 0x1b0128;
constexpr uint32_t kItemCompareRVA = 0x3cc19;
constexpr char     kItemRttiName[] = ".?AVInventoryListViewItem_c@@";

using FnCompare = int(__thiscall*)(void* item, void* other, int column);
static FnCompare g_origCompare = nullptr;
static void*     g_itemVtable  = nullptr;

using FnGetListView     = void*(__thiscall*)(const void* item);       // MultiListViewItem_c::GetListView
using FnGetLayoutMode   = int(__thiscall*)(const void* list);         // MultiListView_c::GetLayoutMode
using FnGetSortOrder    = int(__thiscall*)(const void* list);         // MultiListView_c::GetActiveSortOrder
using FnCanSort         = bool(__thiscall*)(const void* list);        // MultiListView_c::CanSort
using FnSort            = void(__thiscall*)(void* list, bool force);  // MultiListView_c::Sort
using FnVariantIdentity = AOIdentity*(__thiscall*)(const void* variant, AOIdentity* ret);
using FnN3GetInstance   = void*(__cdecl*)();
using FnGetClientInst   = unsigned(__thiscall*)(const void* n3);

static FnGetListView     g_getListView     = nullptr;
static FnGetLayoutMode   g_getLayoutMode   = nullptr;
static FnGetSortOrder    g_getSortOrder    = nullptr;
static FnCanSort         g_canSort         = nullptr;
static FnSort            g_sort            = nullptr;
static FnVariantIdentity g_variantIdentity = nullptr;
static FnN3GetInstance   g_n3GetInstance   = nullptr;
static FnGetClientInst   g_getClientInst   = nullptr;

// ── Layouts (GUI.dll) ──────────────────────────────────────────────────

constexpr uint32_t kItemVariantOffset = 0x20;  // Variant ID (Identity_t)
constexpr uint32_t kItemNameOffset    = 0x4c;  // String, compared for column 1
constexpr int      kLayoutList        = 1;     // MultiListView_c +0x158: 0 grid, 1 list

// ── Pins ───────────────────────────────────────────────────────────────

struct Pin {
    AOIdentity  id;
    std::string name;
};

// Game thread only (GUI sorts and clicks). Index = rank: first pinned first.
static std::vector<Pin> g_pins;
static unsigned         g_pinsChar = 0;   // character the pins belong to
static char             g_pinsPath[MAX_PATH] = {};

static bool IsPinsEnabled() {
    AOVariant v;
    if (GameAPI::GetVariant("AOR_InvPins", v)) {
        if (v.type == static_cast<uint32_t>(VariantType::Bool)) return v.as_bool;
        if (v.type == static_cast<uint32_t>(VariantType::Int))  return v.as_int != 0;
    }
    return true;
}

static bool BuildPinsPath() {
    char exe[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    char* slash = nullptr;
    for (char* p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    if (!slash) return false;
    *(slash + 1) = '\0';
    return _snprintf_s(g_pinsPath, sizeof(g_pinsPath), _TRUNCATE,
                       "%sAOReloadedPins.ini", exe) > 0;
}

static void SectionName(unsigned charId, char* out, size_t size) {
    _snprintf_s(out, size, _TRUNCATE, "Char%u", charId);
}

// Section format, one line per pin in rank order:  TTTTTTTT:IIIIIIII=name
static void LoadPins(unsigned charId) {
    g_pins.clear();
    g_pinsChar = charId;

    char section[32];
    SectionName(charId, section, sizeof(section));
    std::vector<char> buf(32 * 1024);
    DWORD n = GetPrivateProfileSectionA(section, buf.data(),
                                        static_cast<DWORD>(buf.size()), g_pinsPath);
    for (const char* line = buf.data(); n && *line; line += std::strlen(line) + 1) {
        Pin pin = {};
        int nameAt = 0;
        if (std::sscanf(line, "%8x:%8x=%n", &pin.id.type, &pin.id.instance, &nameAt) == 2 &&
            nameAt > 0) {
            pin.name = line + nameAt;
            g_pins.push_back(pin);
        }
    }
    Log("[pins] loaded %u pin(s) for character %u",
        static_cast<unsigned>(g_pins.size()), charId);
}

static void SavePins() {
    char section[32];
    SectionName(g_pinsChar, section, sizeof(section));

    std::string data;
    for (const auto& pin : g_pins) {
        char key[32];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "%08X:%08X=", pin.id.type, pin.id.instance);
        data += key;
        data += pin.name;
        data.push_back('\0');
    }
    data.push_back('\0');
    if (!WritePrivateProfileSectionA(section, data.data(), g_pinsPath))
        Log("[pins] saving to %s failed: %lu", g_pinsPath, GetLastError());
}

// Make sure g_pins belongs to the logged-in character.
static bool EnsurePinsLoaded() {
    void* n3 = g_n3GetInstance();
    const unsigned charId = n3 ? g_getClientInst(n3) : 0;
    if (charId == 0) return false;
    if (charId != g_pinsChar) LoadPins(charId);
    return true;
}

static AOIdentity ItemIdentity(const void* item) {
    AOIdentity id = {};
    g_variantIdentity(static_cast<const uint8_t*>(item) + kItemVariantOffset, &id);
    return id;
}

static const char* ItemName(const void* item) {
    return reinterpret_cast<const AOString*>(
        static_cast<const uint8_t*>(item) + kItemNameOffset)->c_str();
}

static bool IsInventoryItem(const void* item) {
    return item && *static_cast<void* const*>(item) == g_itemVtable;
}

// Pin rank of `item`, or -1. Pins only count while the same item is there.
static int PinRank(const void* item) {
    if (g_pins.empty()) return -1;
    const AOIdentity id = ItemIdentity(item);
    for (size_t i = 0; i < g_pins.size(); ++i) {
        if (SameIdentity(g_pins[i].id, id))
            return g_pins[i].name == ItemName(item) ? static_cast<int>(i) : -1;
    }
    return -1;
}

// ── Compare wrapper ────────────────────────────────────────────────────

static int __fastcall CompareDetour(void* item, void* /*edx*/, void* other, int column) {
    if (IsInventoryItem(other) && IsPinsEnabled() && EnsurePinsLoaded() && !g_pins.empty()) {
        void* list = g_getListView(item);
        if (list && g_getLayoutMode(list) == kLayoutList) {
            const int a = PinRank(item);
            const int b = PinRank(other);
            if ((a >= 0 || b >= 0) && a != b) {
                const bool itemFirst = a >= 0 && (b < 0 || a < b);
                const int ascending = itemFirst ? -1 : 1;
                return g_getSortOrder(list) == 0 ? ascending : -ascending;
            }
        }
    }
    return g_origCompare(item, other, column);
}

// ── Click ──────────────────────────────────────────────────────────────

bool TogglePinFromClick(void* list, void* item) {
    if (!g_origCompare || !IsInventoryItem(item) || !IsPinsEnabled()) return false;
    if (!EnsurePinsLoaded()) {
        Log("[pins] no character yet, ignoring pin click");
        return true;
    }

    const AOIdentity id = ItemIdentity(item);
    const char* name = ItemName(item);
    bool pinned = true;
    for (auto it = g_pins.begin(); it != g_pins.end(); ++it) {
        if (SameIdentity(it->id, id)) {
            // Same slot: unpin, or replace a stale pin from an item that moved away.
            pinned = it->name != name;
            g_pins.erase(it);
            break;
        }
    }
    if (pinned) g_pins.push_back({id, name});
    SavePins();
    Log("[pins] %s %08X:%08X \"%s\", %u pinned", pinned ? "pinned" : "unpinned",
        id.type, id.instance, name, static_cast<unsigned>(g_pins.size()));

    if (g_getLayoutMode(list) == kLayoutList && g_canSort(list)) g_sort(list, true);
    return true;
}

// ── Init ───────────────────────────────────────────────────────────────

template <typename T>
static bool Resolve(T& out, const char* module, const char* name) {
    HMODULE mod = GetModuleHandleA(module);
    out = mod ? reinterpret_cast<T>(GetProcAddress(mod, name)) : nullptr;
    if (!out) Log("[pins] %s!%s not found", module, name);
    return out != nullptr;
}

// vtable[-1] -> RTTI CompleteObjectLocator; +12 -> TypeDescriptor; +8 = name.
static bool VtableIsClass(void** vtable, const char* rttiName) {
    auto* col = static_cast<const uint8_t*>(vtable[-1]);
    if (!col) return false;
    const uint8_t* td = nullptr;
    std::memcpy(&td, col + 12, 4);
    return td && std::strcmp(reinterpret_cast<const char*>(td + 8), rttiName) == 0;
}

bool InitInventoryPins() {
    bool ok = true;
    ok &= Resolve(g_getListView,   "GUI.dll", "?GetListView@MultiListViewItem_c@@QBEPAVMultiListView_c@@XZ");
    ok &= Resolve(g_getLayoutMode, "GUI.dll", "?GetLayoutMode@MultiListView_c@@QBE?AW4LayoutMode_e@1@XZ");
    ok &= Resolve(g_getSortOrder,  "GUI.dll", "?GetActiveSortOrder@MultiListView_c@@ABE?AW4SortOrder_e@1@XZ");
    ok &= Resolve(g_canSort,       "GUI.dll", "?CanSort@MultiListView_c@@QBE_NXZ");
    ok &= Resolve(g_sort,          "GUI.dll", "?Sort@MultiListView_c@@QAEX_N@Z");
    ok &= Resolve(g_variantIdentity, "Utils.dll", "??BVariant@@QBE?AVIdentity_t@@XZ");
    ok &= Resolve(g_n3GetInstance, "Interfaces.dll", "?GetInstance@N3InterfaceModule_t@@SAPAV1@XZ");
    ok &= Resolve(g_getClientInst, "Interfaces.dll", "?GetClientInst@N3InterfaceModule_t@@QBEIXZ");
    if (!ok) return false;

    if (!BuildPinsPath()) {
        Log("[pins] can't build AOReloadedPins.ini path");
        return false;
    }

    auto** vtable = static_cast<void**>(ResolveRVA("GUI.dll", kItemVtableRVA));
    void* compare = ResolveRVA("GUI.dll", kItemCompareRVA);
    if (!vtable || !compare || vtable[1] != compare || !VtableIsClass(vtable, kItemRttiName)) {
        Log("[pins] InventoryListViewItem_c vtable at RVA 0x%X doesn't match (client update?)",
            kItemVtableRVA);
        return false;
    }

    // Publish the original before the slot points at the wrapper.
    g_origCompare = reinterpret_cast<FnCompare>(compare);
    g_itemVtable  = vtable;

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[1], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log("[pins] VirtualProtect failed: %lu", GetLastError());
        g_origCompare = nullptr;
        return false;
    }
    InterlockedExchangePointer(&vtable[1], reinterpret_cast<void*>(&CompareDetour));
    DWORD ignored = 0;
    VirtualProtect(&vtable[1], sizeof(void*), oldProtect, &ignored);

    Log("[pins] ctrl+alt+click pinning installed (pins in %s)", g_pinsPath);
    return true;
}

}  // namespace aor
