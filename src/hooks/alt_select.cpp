// alt_select.cpp — Alt+click multi-select, moved together.
//
// See alt_select.h for the design overview and docs/inventory_trade_clicks.md
// for the click path this hooks into.

#include "hooks/alt_select.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "ao/types.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
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

// Inventory list item mouse-up slot: the list connects its own
// MultiListView_c signal +0x134 (fired by MultiListView_c::MouseUp) to it.
// __thiscall, RET 0x10: (void* unknown, MultiListViewItem_c* item,
// int button, Message* drag) — drag is non-null when an item was dropped.
// GUI.dll RVA 0x40a17, not exported. It starts with an SEH prolog,
//   mov eax, <EH table>   ; B8 + absolute address (relocated by the loader)
//   call __EH_prolog      ; E8 + relative offset
//   sub esp, ...          ; 83 EC
// so the check compares the address against its relocated value.
constexpr uint32_t kListMouseUpRVA   = 0x40a17;
constexpr uint32_t kListMouseUpEHRVA = 0x17fe7c;
constexpr uint8_t  kListMouseUpTail[] = {0xE8, 0x83, 0x2E, 0x13, 0x00, 0x83, 0xEC};

using FnListMouseUp = void(__thiscall*)(void* list, void* unknown, void* item,
                                        int button, void* drag);
static FnListMouseUp g_origListMouseUp = nullptr;

// MultiListView_c::~MultiListView_c — to forget a list before it's freed.
using FnListDtor = void(__thiscall*)(void* list);
static FnListDtor g_origListDtor = nullptr;

// N3InterfaceModule_t move calls (Interfaces.dll, exported).
using FnMoveTo   = void(__thiscall*)(const void* n3, const AOIdentity* target,
                                     const AOIdentity* item);
using FnMoveHome = bool(__thiscall*)(const void* n3, const AOIdentity* item);
static FnMoveTo   g_origContainerAdd = nullptr;
static FnMoveTo   g_origTradeAdd     = nullptr;
static FnMoveTo   g_origTradeRemove  = nullptr;
static FnMoveHome g_origMoveHome     = nullptr;

using FnGetQualifiers   = unsigned(__thiscall*)(const void* view);            // View::GetQualifiers
using FnItemSelect      = void(__thiscall*)(void* item, bool selected, bool notify);
using FnItemInvalidate  = void(__thiscall*)(void* item);
using FnVariantIdentity = AOIdentity*(__thiscall*)(const void* variant, AOIdentity* ret);

static FnGetQualifiers   g_getQualifiers   = nullptr;
static FnItemSelect      g_itemSelect      = nullptr;
static FnItemInvalidate  g_itemInvalidate  = nullptr;
static FnVariantIdentity g_variantIdentity = nullptr;

// ── Layouts (GUI.dll) ──────────────────────────────────────────────────

// MultiListViewItem_c: Variant ID (holds the item's Identity_t).
constexpr uint32_t kItemVariantOffset = 0x20;
// MultiListView_c: std::vector<MultiListViewItem_c*> (GetItemList), MSVC 2010
// layout {first, last, end} — GetItemCount() is (+0x1bc - +0x1b8) / 4.
constexpr uint32_t kListItemsOffset = 0x1b8;

// Qualifier bits from WindowController_c::GetQualifiers: LAlt 0x10, RAlt 0x20.
constexpr unsigned kQualifierAlt = 0x30;

// ── Selection ──────────────────────────────────────────────────────────

// All touched on the game thread only (GUI slots, N3 calls from the GUI).
// g_selList is only dereferenced while alive: the destructor hook clears it.
static std::vector<AOIdentity> g_selection;
static void*                   g_selList  = nullptr;
static bool                    g_replaying = false;

static bool IsAltSelectEnabled() {
    AOVariant v;
    if (GameAPI::GetVariant("AOR_AltSelect", v)) {
        if (v.type == static_cast<uint32_t>(VariantType::Bool)) return v.as_bool;
        if (v.type == static_cast<uint32_t>(VariantType::Int))  return v.as_int != 0;
    }
    return true;
}

static AOIdentity ItemIdentity(void* item) {
    AOIdentity id = {};
    g_variantIdentity(static_cast<uint8_t*>(item) + kItemVariantOffset, &id);
    return id;
}

static int FindSelected(const AOIdentity& id) {
    for (size_t i = 0; i < g_selection.size(); ++i)
        if (SameIdentity(g_selection[i], id)) return static_cast<int>(i);
    return -1;
}

static void Highlight(void* item, bool on) {
    g_itemSelect(item, on, false);
    g_itemInvalidate(item);
}

// Remove all highlights in the selection's list, then forget the selection.
static void ClearSelection() {
    if (g_selList && !g_selection.empty()) {
        auto* p = static_cast<uint8_t*>(g_selList) + kListItemsOffset;
        void** first = *reinterpret_cast<void***>(p);
        void** last  = *reinterpret_cast<void***>(p + 4);
        for (void** it = first; first && it < last; ++it) {
            if (*it && FindSelected(ItemIdentity(*it)) >= 0)
                Highlight(*it, false);
        }
    }
    g_selection.clear();
    g_selList = nullptr;
}

static void ToggleItem(void* list, void* item) {
    if (list != g_selList) {
        ClearSelection();   // one list at a time
        g_selList = list;
    }

    const AOIdentity id = ItemIdentity(item);
    const int idx = FindSelected(id);
    if (idx >= 0) {
        g_selection.erase(g_selection.begin() + idx);
        Highlight(item, false);
    } else {
        g_selection.push_back(id);
        Highlight(item, true);
    }
    Log("[altsel] %s %08X:%08X, %u selected", idx >= 0 ? "deselected" : "selected",
        id.type, id.instance, static_cast<unsigned>(g_selection.size()));
}

// ── Hooks ──────────────────────────────────────────────────────────────

static void __fastcall ListMouseUpDetour(
        void* list, void* /*edx*/, void* unknown, void* item, int button, void* drag) {
    if (!drag && IsAltSelectEnabled() && (g_getQualifiers(list) & kQualifierAlt)) {
        if (item) {
            ToggleItem(list, item);
        } else if (list == g_selList) {
            ClearSelection();
            Log("[altsel] selection cleared");
        }
        return;  // Alt+click never reaches the stock use/move handlers
    }
    g_origListMouseUp(list, unknown, item, button, drag);
}

static void __fastcall ListDtorDetour(void* list, void* /*edx*/) {
    if (list == g_selList) {
        // Items go away with the list; nothing left to un-highlight.
        g_selection.clear();
        g_selList = nullptr;
    }
    g_origListDtor(list);
}

// If `item` is selected, returns the other selected items and clears the
// selection (the caller then moves them); otherwise returns false.
static bool TakeRestOfSelection(const AOIdentity* item, std::vector<AOIdentity>& rest) {
    if (g_replaying || !item || FindSelected(*item) < 0) return false;
    for (const auto& id : g_selection)
        if (!SameIdentity(id, *item)) rest.push_back(id);
    ClearSelection();
    return true;
}

static void ReplayMoveTo(FnMoveTo orig, const char* what, const void* n3,
                         const AOIdentity* target, const AOIdentity* item) {
    std::vector<AOIdentity> rest;
    const bool group = TakeRestOfSelection(item, rest);
    orig(n3, target, item);
    if (!group) return;

    g_replaying = true;
    for (const auto& id : rest) orig(n3, target, &id);
    g_replaying = false;
    Log("[altsel] %s: moved %u more selected item(s) to %08X:%08X", what,
        static_cast<unsigned>(rest.size()), target ? target->type : 0,
        target ? target->instance : 0);
}

static void __fastcall ContainerAddDetour(const void* n3, void*, const AOIdentity* target,
                                          const AOIdentity* item) {
    ReplayMoveTo(g_origContainerAdd, "container", n3, target, item);
}

static void __fastcall TradeAddDetour(const void* n3, void*, const AOIdentity* target,
                                      const AOIdentity* item) {
    ReplayMoveTo(g_origTradeAdd, "trade add", n3, target, item);
}

static void __fastcall TradeRemoveDetour(const void* n3, void*, const AOIdentity* target,
                                         const AOIdentity* item) {
    ReplayMoveTo(g_origTradeRemove, "trade remove", n3, target, item);
}

static bool __fastcall MoveHomeDetour(const void* n3, void*, const AOIdentity* item) {
    std::vector<AOIdentity> rest;
    const bool group = TakeRestOfSelection(item, rest);
    const bool ok = g_origMoveHome(n3, item);
    if (group) {
        g_replaying = true;
        for (const auto& id : rest) g_origMoveHome(n3, &id);
        g_replaying = false;
        Log("[altsel] to inventory: moved %u more selected item(s)",
            static_cast<unsigned>(rest.size()));
    }
    return ok;
}

// ── Init ───────────────────────────────────────────────────────────────

template <typename T>
static bool Resolve(T& out, const char* module, const char* name) {
    HMODULE mod = GetModuleHandleA(module);
    out = mod ? reinterpret_cast<T>(GetProcAddress(mod, name)) : nullptr;
    if (!out) Log("[altsel] %s!%s not found", module, name);
    return out != nullptr;
}

template <typename T>
static bool Hook(void* target, void* detour, T& orig, const char* what) {
    void* tramp = nullptr;
    if (!InstallHook(target, detour, &tramp)) {
        Log("[altsel] %s hook failed", what);
        return false;
    }
    orig = reinterpret_cast<T>(tramp);
    return true;
}

static bool ListMouseUpMatches(const uint8_t* code) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA("GUI.dll"));
    uint32_t imm = 0;
    std::memcpy(&imm, code + 1, 4);
    return code[0] == 0xB8 && imm == base + kListMouseUpEHRVA &&
           std::memcmp(code + 5, kListMouseUpTail, sizeof(kListMouseUpTail)) == 0;
}

bool InitAltSelect() {
    bool ok = true;
    ok &= Resolve(g_getQualifiers,   "GUI.dll", "?GetQualifiers@View@@QBEIXZ");
    ok &= Resolve(g_itemSelect,      "GUI.dll", "?Select@MultiListViewItem_c@@QAEX_N0@Z");
    ok &= Resolve(g_itemInvalidate,  "GUI.dll", "?Invalidate@MultiListViewItem_c@@QAEXXZ");
    ok &= Resolve(g_variantIdentity, "Utils.dll", "??BVariant@@QBE?AVIdentity_t@@XZ");

    void* dtor = nullptr;
    void* containerAdd = nullptr;
    void* tradeAdd = nullptr;
    void* tradeRemove = nullptr;
    void* moveHome = nullptr;
    ok &= Resolve(dtor, "GUI.dll", "??1MultiListView_c@@UAE@XZ");
    ok &= Resolve(containerAdd, "Interfaces.dll",
                  "?N3Msg_ContainerAddItem@N3InterfaceModule_t@@QBEXABVIdentity_t@@0@Z");
    ok &= Resolve(tradeAdd, "Interfaces.dll",
                  "?N3Msg_TradeAddItem@N3InterfaceModule_t@@QBEXABVIdentity_t@@0@Z");
    ok &= Resolve(tradeRemove, "Interfaces.dll",
                  "?N3Msg_TradeRemoveItem@N3InterfaceModule_t@@QBEXABVIdentity_t@@0@Z");
    ok &= Resolve(moveHome, "Interfaces.dll",
                  "?MoveItemToInventory@N3InterfaceModule_t@@QBE_NABVIdentity_t@@@Z");
    if (!ok) return false;

    void* mouseUp = ResolveRVA("GUI.dll", kListMouseUpRVA);
    if (!mouseUp || !ListMouseUpMatches(static_cast<const uint8_t*>(mouseUp))) {
        Log("[altsel] list mouse-up at RVA 0x%X doesn't match (client update?)", kListMouseUpRVA);
        return false;
    }

    // Destructor and move hooks before the click hook, so a selection can
    // never exist without its safety nets.
    if (!Hook(dtor,         reinterpret_cast<void*>(&ListDtorDetour),     g_origListDtor,     "list destructor")) return false;
    if (!Hook(containerAdd, reinterpret_cast<void*>(&ContainerAddDetour), g_origContainerAdd, "ContainerAddItem")) return false;
    if (!Hook(tradeAdd,     reinterpret_cast<void*>(&TradeAddDetour),     g_origTradeAdd,     "TradeAddItem")) return false;
    if (!Hook(tradeRemove,  reinterpret_cast<void*>(&TradeRemoveDetour),  g_origTradeRemove,  "TradeRemoveItem")) return false;
    if (!Hook(moveHome,     reinterpret_cast<void*>(&MoveHomeDetour),     g_origMoveHome,     "MoveItemToInventory")) return false;
    if (!Hook(mouseUp,      reinterpret_cast<void*>(&ListMouseUpDetour),  g_origListMouseUp,  "list mouse-up")) return false;

    Log("[altsel] alt+click multi-select installed");
    return true;
}

}  // namespace aor
