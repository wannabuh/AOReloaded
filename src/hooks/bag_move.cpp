// bag_move.cpp — Ctrl+click inventory -> backpack.
//
// See bag_move.h for the design overview and docs/inventory_trade_clicks.md
// for how the stock click handling was worked out.

#include "hooks/bag_move.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "ao/types.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace aor {

// Identity_t: 8 bytes, { type, instance }.
struct AOIdentity {
    uint32_t type;
    uint32_t instance;
};
static_assert(sizeof(AOIdentity) == 8, "Identity_t is 8 bytes");

// ── Game functions ─────────────────────────────────────────────────────

// InventoryViewBase_c item-activation slot, connected to signal +0x2e0 of
// the view's item list (fires on a plain click; Shift+click goes elsewhere).
// __thiscall, RET 8: (void* unknown, MultiListViewItem_c* item).
// GUI.dll RVA 0xca1e7; checked against these bytes before hooking since the
// function isn't exported:
//   push ebp; mov ebp,esp; push ecx; push ecx; push esi; push edi;
//   mov edi,[ebp+0xC]; mov esi,ecx
constexpr uint32_t kItemActivatedRVA = 0xca1e7;
constexpr uint8_t  kItemActivatedSig[] = {
    0x55, 0x8B, 0xEC, 0x51, 0x51, 0x56, 0x57, 0x8B, 0x7D, 0x0C, 0x8B, 0xF1};

using FnItemActivated = void(__thiscall*)(void* view, void* unknown, void* item);
static FnItemActivated g_origItemActivated = nullptr;

// InventoryGUIModule_c::SlotContainerOpened(Identity_t const&, bool, bool)
// and ::SlotContainerClosed(Identity_t const&) — private but exported.
using FnContainerOpened = void(__thiscall*)(void* module, const AOIdentity* id, bool a, bool b);
using FnContainerClosed = void(__thiscall*)(void* module, const AOIdentity* id);
static FnContainerOpened g_origContainerOpened = nullptr;
static FnContainerClosed g_origContainerClosed = nullptr;

using FnGetModule       = void*(__cdecl*)();                  // InventoryGUIModule_c::GetInstanceIfAny
using FnGetTradeView    = void*(__thiscall*)(void* module);   // InventoryGUIModule_c::GetTradeView
using FnGetQualifiers   = unsigned(__thiscall*)(const void* view);  // View::GetQualifiers
using FnN3GetInstance   = void*(__cdecl*)();                  // N3InterfaceModule_t::GetInstance
using FnContainerAdd    = void(__thiscall*)(const void* n3, const AOIdentity* container,
                                            const AOIdentity* item);
// Variant::operator Identity_t() const — returns by value through a hidden pointer.
using FnVariantIdentity = AOIdentity*(__thiscall*)(const void* variant, AOIdentity* ret);

static FnGetModule       g_getModule       = nullptr;
static FnGetTradeView    g_getTradeView    = nullptr;
static FnGetQualifiers   g_getQualifiers   = nullptr;
static FnN3GetInstance   g_n3GetInstance   = nullptr;
static FnContainerAdd    g_containerAdd    = nullptr;
static FnVariantIdentity g_variantIdentity = nullptr;

// ── Layouts (GUI.dll, see docs/inventory_trade_clicks.md) ──────────────

// InventoryViewBase_c: the container this view shows, and a flag that is
// set for the main inventory. The stock slot uses the item on a click
// (N3Msg_UseItem) exactly when flag && container.type != 0xDEAD, and moves
// it to the main inventory otherwise — so that condition is "main inventory".
constexpr uint32_t kViewContainerOffset = 0x140;
constexpr uint32_t kViewMainFlagOffset  = 0x14c;
constexpr uint32_t kInvalidType         = 0xDEAD;

// MultiListViewItem_c: a byte the stock slot checks first (ignores the
// click if set), and the Variant holding the item's Identity_t.
constexpr uint32_t kItemIgnoreOffset  = 0xf0;
constexpr uint32_t kItemVariantOffset = 0x20;

// Qualifier bits from WindowController_c::GetQualifiers: LCtrl 0x4, RCtrl 0x8.
constexpr unsigned kQualifierCtrl = 0xC;

// ── Open container tracking ────────────────────────────────────────────

// Oldest first; the target is the back. Only touched on the game thread
// (GUI slots and clicks), so no locking.
static std::vector<AOIdentity> g_openContainers;

static bool SameIdentity(const AOIdentity& a, const AOIdentity& b) {
    return a.type == b.type && a.instance == b.instance;
}

static void ForgetContainer(const AOIdentity& id) {
    for (auto it = g_openContainers.begin(); it != g_openContainers.end(); ) {
        if (SameIdentity(*it, id)) it = g_openContainers.erase(it);
        else ++it;
    }
}

static void __fastcall ContainerOpenedDetour(
        void* module, void* /*edx*/, const AOIdentity* id, bool a, bool b) {
    g_origContainerOpened(module, id, a, b);
    if (!id) return;

    ForgetContainer(*id);  // re-opening moves it to the back
    g_openContainers.push_back(*id);
    Log("[bagmove] container opened %08X:%08X (%d, %d), %u open",
        id->type, id->instance, a ? 1 : 0, b ? 1 : 0,
        static_cast<unsigned>(g_openContainers.size()));
}

static void __fastcall ContainerClosedDetour(
        void* module, void* /*edx*/, const AOIdentity* id) {
    if (id) {
        ForgetContainer(*id);
        Log("[bagmove] container closed %08X:%08X, %u open",
            id->type, id->instance, static_cast<unsigned>(g_openContainers.size()));
    }
    g_origContainerClosed(module, id);
}

// ── Click handling ─────────────────────────────────────────────────────

static bool IsBagMoveEnabled() {
    AOVariant v;
    if (GameAPI::GetVariant("AOR_BagMove", v)) {
        if (v.type == static_cast<uint32_t>(VariantType::Bool)) return v.as_bool;
        if (v.type == static_cast<uint32_t>(VariantType::Int))  return v.as_int != 0;
    }
    return true;
}

static bool IsTradeOpen() {
    void* module = g_getModule();
    return module && g_getTradeView(module);
}

static bool IsMainInventoryView(const void* view) {
    auto* p = static_cast<const uint8_t*>(view);
    auto* container = reinterpret_cast<const AOIdentity*>(p + kViewContainerOffset);
    return p[kViewMainFlagOffset] != 0 && container->type != kInvalidType;
}

// Returns true if the click was handled here.
static bool TryMoveToBackpack(void* view, void* item) {
    if (!item || g_openContainers.empty()) return false;
    if (static_cast<const uint8_t*>(item)[kItemIgnoreOffset]) return false;
    if (!(g_getQualifiers(view) & kQualifierCtrl)) return false;
    if (IsTradeOpen()) return false;              // stock: Ctrl+click sells / trades
    if (!IsMainInventoryView(view)) return false; // stock: moves out of the backpack
    if (!IsBagMoveEnabled()) return false;

    AOIdentity itemId = {};
    g_variantIdentity(static_cast<const uint8_t*>(item) + kItemVariantOffset, &itemId);

    const AOIdentity target = g_openContainers.back();
    void* n3 = g_n3GetInstance();
    if (!n3) return false;

    Log("[bagmove] item %08X:%08X -> container %08X:%08X",
        itemId.type, itemId.instance, target.type, target.instance);
    g_containerAdd(n3, &target, &itemId);
    return true;
}

static void __fastcall ItemActivatedDetour(
        void* view, void* /*edx*/, void* unknown, void* item) {
    if (TryMoveToBackpack(view, item)) return;
    g_origItemActivated(view, unknown, item);
}

// ── Init ───────────────────────────────────────────────────────────────

template <typename T>
static bool Resolve(T& out, const char* module, const char* name) {
    HMODULE mod = GetModuleHandleA(module);
    out = mod ? reinterpret_cast<T>(GetProcAddress(mod, name)) : nullptr;
    if (!out) Log("[bagmove] %s!%s not found", module, name);
    return out != nullptr;
}

bool InitBagMove() {
    bool ok = true;
    ok &= Resolve(g_getModule,       "GUI.dll", "?GetInstanceIfAny@InventoryGUIModule_c@@SAPAV1@XZ");
    ok &= Resolve(g_getTradeView,    "GUI.dll", "?GetTradeView@InventoryGUIModule_c@@QAEPAVDockableView_c@@XZ");
    ok &= Resolve(g_getQualifiers,   "GUI.dll", "?GetQualifiers@View@@QBEIXZ");
    ok &= Resolve(g_n3GetInstance,   "Interfaces.dll", "?GetInstance@N3InterfaceModule_t@@SAPAV1@XZ");
    ok &= Resolve(g_containerAdd,    "Interfaces.dll",
                  "?N3Msg_ContainerAddItem@N3InterfaceModule_t@@QBEXABVIdentity_t@@0@Z");
    ok &= Resolve(g_variantIdentity, "Utils.dll", "??BVariant@@QBE?AVIdentity_t@@XZ");

    void* opened = nullptr;
    void* closed = nullptr;
    ok &= Resolve(opened, "GUI.dll", "?SlotContainerOpened@InventoryGUIModule_c@@AAEXABVIdentity_t@@_N1@Z");
    ok &= Resolve(closed, "GUI.dll", "?SlotContainerClosed@InventoryGUIModule_c@@AAEXABVIdentity_t@@@Z");
    if (!ok) return false;

    void* slot = ResolveRVA("GUI.dll", kItemActivatedRVA);
    if (!slot || std::memcmp(slot, kItemActivatedSig, sizeof(kItemActivatedSig)) != 0) {
        Log("[bagmove] item slot at RVA 0x%X doesn't match (client update?)", kItemActivatedRVA);
        return false;
    }

    // Tracking hooks first, so no open/close is missed once clicks are handled.
    void* tramp = nullptr;
    if (!InstallHook(opened, reinterpret_cast<void*>(&ContainerOpenedDetour), &tramp)) {
        Log("[bagmove] SlotContainerOpened hook failed");
        return false;
    }
    g_origContainerOpened = reinterpret_cast<FnContainerOpened>(tramp);

    if (!InstallHook(closed, reinterpret_cast<void*>(&ContainerClosedDetour), &tramp)) {
        Log("[bagmove] SlotContainerClosed hook failed");
        return false;
    }
    g_origContainerClosed = reinterpret_cast<FnContainerClosed>(tramp);

    if (!InstallHook(slot, reinterpret_cast<void*>(&ItemActivatedDetour), &tramp)) {
        Log("[bagmove] item slot hook failed");
        return false;
    }
    g_origItemActivated = reinterpret_cast<FnItemActivated>(tramp);

    Log("[bagmove] ctrl+click inventory -> backpack installed");
    return true;
}

}  // namespace aor
