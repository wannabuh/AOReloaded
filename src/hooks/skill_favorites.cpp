// skill_favorites.cpp — Favorites group and Shift+click ±5 in the skills window.
//
// See skill_favorites.h for the design overview and src/ao/CLAUDE.md
// ("Skills window") for the SkillWindow / StatRow internals this relies on.

#include "hooks/skill_favorites.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "ao/types.h"
#include "core/logging.h"
#include "core/settings.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace aor {

// ── GUI.dll functions (not exported) ───────────────────────────────────
//
// All hooked ones start with the MSVC SEH prolog
//   mov eax, <EH table>   ; B8 + absolute address (relocated by the loader)
//   call __EH_prolog      ; E8 + relative offset to GUI.dll RVA 0x1738a4
// so the check compares both against their expected values.

struct SehFunction {
    uint32_t    rva;
    uint32_t    ehTableRVA;
    const char* name;
};

constexpr uint32_t kEhPrologRVA = 0x1738a4;

// SkillWindow::SkillWindow(), __thiscall, RET.
constexpr SehFunction kWindowCtor  = {0xfc18e, 0x1998a8, "SkillWindow ctor"};
// SkillWindow::InitGroups(), __thiscall, RET: fills the group table.
constexpr SehFunction kInitGroups  = {0xfb596, 0x199683, "group table init"};
// SkillWindow::DistributeIP(std::vector<int>* changed), __thiscall, RET 4:
// the "Suggested IP distribution" button's work.
constexpr SehFunction kSuggestIP   = {0xfac49, 0x1993e3, "suggested IP"};
// SkillWindow::OnRowClicked(ButtonBase_c* row), __thiscall, RET 4: slot for
// every StatRow's click signal (+0x148); shows the skill's details.
constexpr SehFunction kRowClicked  = {0xf97a6, 0x199275, "row clicked"};
// StatRow::StatRow(Stat_e, bool full, bool disabled), __thiscall, RET 0xC.
constexpr SehFunction kRowCtor     = {0xfe956, 0x199c56, "StatRow ctor"};
// StatRow::~StatRow(), __thiscall, RET.
constexpr SehFunction kRowDtor     = {0xff3f4, 0x199c84, "StatRow dtor"};
// bool StatRow::ChangeDelta(int points), __thiscall, RET 4: adds to the
// pending points, charges the IP difference through the row's +0x188
// signal (backing off while it can't be paid), redraws.
constexpr SehFunction kChangeDelta = {0xfde49, 0x19995d, "ChangeDelta"};
// StatRow + / - button slots, __thiscall, RET. Pressed: +/-1 and start the
// auto-repeat timer. Released: stop it, and with any qualifier key go to
// the cap / to zero. Both fire the row's click signal.
constexpr SehFunction kIncPressed  = {0xfe261, 0x18965e, "increase pressed"};
constexpr SehFunction kIncReleased = {0xfe361, 0x18af75, "increase released"};
constexpr SehFunction kDecPressed  = {0xfe0ca, 0x18965e, "decrease pressed"};
constexpr SehFunction kDecReleased = {0xfe1b4, 0x18af75, "decrease released"};

// std::vector helpers instantiated in GUI.dll, __thiscall, RET 4. `this` is
// the vector (first, last, end, allocator). Checked by their first bytes.
struct PlainFunction {
    uint32_t    rva;
    uint8_t     sig[8];
    const char* name;
};
// vector<Group>::push_back(Group const&)
constexpr PlainFunction kGroupPushBack = {
    0xff7cc, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B, 0x4E}, "vector<Group>::push_back"};
// vector<vector<Stat_e>>::resize(size_t)
constexpr PlainFunction kStatListsResize = {
    0xff558, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B, 0x4E}, "vector<vector<int>>::resize"};
// vector<Stat_e>::push_back(Stat_e const&)
constexpr PlainFunction kStatPushBack = {
    0x64bee, {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B, 0x46}, "vector<int>::push_back"};

using FnThis        = void(__thiscall*)(void* self);
using FnCtorThis    = void*(__thiscall*)(void* self);
using FnRowCtor     = void*(__thiscall*)(void* row, int stat, int full, int disabled);
using FnChangeDelta = bool(__thiscall*)(void* row, int points);
using FnWithArg     = void(__thiscall*)(void* self, void* arg);
using FnPushBack    = void(__thiscall*)(void* vec, const void* value);
using FnResize      = void(__thiscall*)(void* vec, size_t count);

static FnCtorThis    g_origWindowCtor  = nullptr;
static FnThis        g_origInitGroups  = nullptr;
static FnWithArg     g_origSuggestIP   = nullptr;
static FnWithArg     g_origRowClicked  = nullptr;
static FnRowCtor     g_origRowCtor     = nullptr;
static FnThis        g_origRowDtor     = nullptr;
static FnChangeDelta g_origChangeDelta = nullptr;
static FnThis        g_origIncPressed  = nullptr;
static FnThis        g_origIncReleased = nullptr;
static FnThis        g_origDecPressed  = nullptr;
static FnThis        g_origDecReleased = nullptr;
static FnPushBack    g_groupPushBack   = nullptr;
static FnResize      g_statListsResize = nullptr;
static FnPushBack    g_statPushBack    = nullptr;

// Exported helpers.
using FnGetQualifiers = unsigned(__thiscall*)(const void* view);              // View::GetQualifiers
using FnShow          = void(__thiscall*)(void* view, bool show, bool relayout);  // View::Show
using FnFindChild     = void*(__thiscall*)(void* view, const char* name, bool recursive);
using FnGetText       = const AOString*(__thiscall*)(const void* textView);   // TextView_c::GetTextBuffer
using FnSetText       = void(__thiscall*)(void* textView, const AOString* text);
using FnStringAssign  = void*(__thiscall*)(void* str, const char* value);     // String::operator=
using FnStringDtor    = void(__thiscall*)(void* str);                         // String::~String
using FnN3GetInstance = void*(__cdecl*)();
using FnGetClientInst = unsigned(__thiscall*)(const void* n3);

static FnGetQualifiers g_getQualifiers = nullptr;
static FnShow          g_show          = nullptr;
static FnFindChild     g_findChild     = nullptr;
static FnGetText       g_getText       = nullptr;
static FnSetText       g_setText       = nullptr;
static FnStringAssign  g_stringAssign  = nullptr;
static FnStringDtor    g_stringDtor    = nullptr;
static FnN3GetInstance g_n3GetInstance = nullptr;
static FnGetClientInst g_getClientInst = nullptr;

// ── Layouts (GUI.dll) ──────────────────────────────────────────────────

// SkillWindow
constexpr uint32_t kWindowRootView  = 0x74;  // View*, the loaded Skills.xml
constexpr uint32_t kWindowGroups    = 0x7c;  // std::vector<Group>
constexpr uint32_t kWindowStatLists = 0x8c;  // std::vector<std::vector<Stat_e>>, parallel
constexpr uint32_t kGroupSize       = 0x38;  // String label; String name at +0x1c
constexpr uint32_t kGroupName       = 0x1c;
constexpr uint32_t kStatListSize    = 0x10;  // std::vector<int>
constexpr size_t   kStockGroupCount = 11;    // abilities .. traderepair, disabled

// StatRow (derives from ButtonBase_c at offset 0)
constexpr uint32_t kRowNameLabel = 0x198;  // TextView_c*, the skill name
constexpr uint32_t kRowStat      = 0x1a4;  // Stat_e
constexpr uint32_t kRowBase      = 0x1ac;  // base skill
constexpr uint32_t kRowPending   = 0x1b0;  // points added, not saved yet
constexpr uint32_t kRowCap       = 0x1b4;  // highest base skill allowed
constexpr uint32_t kRowCharged   = 0x1b8;  // IP already charged for kRowPending
constexpr uint32_t kRowFull      = 0x1c4;  // bool: full view row ("_row_ext")
constexpr uint32_t kRowDisabled  = 0x1cc;  // bool: deprecated skill

// Qualifier bits from WindowController_c::GetQualifiers.
constexpr unsigned kQualifierShift = 0x3;
constexpr unsigned kQualifierCtrl  = 0xC;
constexpr unsigned kQualifierAlt   = 0x30;

constexpr char kGroupLabel[]  = "Favorites";
constexpr char kGroupKey[]    = "favorites";
constexpr char kFavMarker[]   = "* ";
constexpr int  kShiftStep     = 5;

template <typename T>
static T& Field(void* obj, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + offset);
}

// ── Settings ───────────────────────────────────────────────────────────

static bool IsEnabled(const char* dvalue) {
    AOVariant v;
    if (GameAPI::GetVariant(dvalue, v)) {
        if (v.type == static_cast<uint32_t>(VariantType::Bool)) return v.as_bool;
        if (v.type == static_cast<uint32_t>(VariantType::Int))  return v.as_int != 0;
    }
    return true;
}

static bool FavoritesEnabled() { return IsEnabled("AOR_SkillFavs"); }
static bool ShiftStepEnabled() { return IsEnabled("AOR_SkillShift"); }

// ── Favorites (per character) ──────────────────────────────────────────

// Game thread only (the skills window).
static std::vector<int> g_favorites;
static unsigned         g_favoritesChar = 0;
static char             g_favoritesPath[MAX_PATH] = {};

static bool BuildFavoritesPath() {
    char exe[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    char* slash = nullptr;
    for (char* p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    if (!slash) return false;
    *(slash + 1) = '\0';
    return _snprintf_s(g_favoritesPath, sizeof(g_favoritesPath), _TRUNCATE,
                       "%sAOReloadedSkills.ini", exe) > 0;
}

static void SectionName(unsigned charId, char* out, size_t size) {
    _snprintf_s(out, size, _TRUNCATE, "Char%u", charId);
}

// [Char<id>]  Favorites=16,17,152
static void LoadFavorites(unsigned charId) {
    g_favorites.clear();
    g_favoritesChar = charId;

    char section[32];
    SectionName(charId, section, sizeof(section));
    char buf[2048] = {};
    GetPrivateProfileStringA(section, "Favorites", "", buf, sizeof(buf), g_favoritesPath);
    for (const char* p = buf; *p;) {
        char* end = nullptr;
        const long stat = std::strtol(p, &end, 10);
        if (end == p) break;
        if (stat > 0) g_favorites.push_back(static_cast<int>(stat));
        p = *end == ',' ? end + 1 : end;
    }
    Log("[skillfav] loaded %u favorite(s) for character %u",
        static_cast<unsigned>(g_favorites.size()), charId);
}

static void SaveFavorites() {
    char section[32];
    SectionName(g_favoritesChar, section, sizeof(section));
    std::string value;
    for (int stat : g_favorites) {
        if (!value.empty()) value += ',';
        value += std::to_string(stat);
    }
    if (!WritePrivateProfileStringA(section, "Favorites", value.c_str(), g_favoritesPath))
        Log("[skillfav] saving to %s failed: %lu", g_favoritesPath, GetLastError());
}

// Make sure g_favorites belongs to the logged-in character.
static bool EnsureFavoritesLoaded() {
    void* n3 = g_n3GetInstance();
    const unsigned charId = n3 ? g_getClientInst(n3) : 0;
    if (charId == 0) return false;
    if (charId != g_favoritesChar) LoadFavorites(charId);
    return true;
}

static bool IsFavorite(int stat) {
    return std::find(g_favorites.begin(), g_favorites.end(), stat) != g_favorites.end();
}

// ── Rows ───────────────────────────────────────────────────────────────

struct RowInfo {
    void* row;
    int   stat;
    bool  full;       // full view row (vs the compact list)
    bool  favorites;  // lives in the favorites group
};

// Every live StatRow, in creation order. Only the skills window makes them.
static std::vector<RowInfo> g_rows;

static RowInfo* FindRow(const void* row) {
    for (auto& info : g_rows)
        if (info.row == row) return &info;
    return nullptr;
}

static void SetFavoriteMarker(void* row, bool favorite) {
    void* label = Field<void*>(row, kRowNameLabel);
    const AOString* current = label ? g_getText(label) : nullptr;
    if (!current) return;

    std::string text = current->c_str();
    const bool marked = text.compare(0, sizeof(kFavMarker) - 1, kFavMarker) == 0;
    if (marked == favorite) return;
    text = favorite ? kFavMarker + text : text.substr(sizeof(kFavMarker) - 1);

    AOString str = AOString::FromShort("");
    g_stringAssign(&str, text.c_str());
    g_setText(label, &str);
    g_stringDtor(&str);
}

// Marker on every row of the skill; favorites group rows shown or hidden.
static void ApplyFavorite(const RowInfo& info, bool favorite) {
    SetFavoriteMarker(info.row, favorite);
    if (info.favorites) g_show(info.row, favorite, true);
}

// ── Keeping the rows of a skill in step ────────────────────────────────

static bool g_syncing = false;
static bool g_swallowChangeDelta = false;

// Give the other rows of `row`'s skill in the same view its pending points.
// Setting the points and the IP already charged for them, then a zero
// change, is how the stock view switch copies them: a zero change redraws
// and recomputes the charge without touching the IP counter.
static void SyncTwins(void* row) {
    const int  stat     = Field<int>(row, kRowStat);
    const bool full     = Field<bool>(row, kRowFull);
    const int  pending  = Field<int>(row, kRowPending);
    const int  charged  = Field<int>(row, kRowCharged);
    for (size_t i = 0; i < g_rows.size(); ++i) {
        void* twin = g_rows[i].row;
        if (twin == row || g_rows[i].stat != stat || g_rows[i].full != full) continue;
        if (Field<int>(twin, kRowPending) == pending && Field<int>(twin, kRowCharged) == charged)
            continue;
        Field<int>(twin, kRowPending) = pending;
        Field<int>(twin, kRowCharged) = charged;
        g_syncing = true;
        g_origChangeDelta(twin, 0);
        g_syncing = false;
    }
}

static bool __fastcall ChangeDeltaDetour(void* row, void* /*edx*/, int points) {
    if (g_swallowChangeDelta) {  // one call: the released slot's jump
        g_swallowChangeDelta = false;
        return true;
    }
    const bool result = g_origChangeDelta(row, points);
    if (!g_syncing) SyncTwins(row);
    return result;
}

static void* __fastcall RowCtorDetour(void* row, void* /*edx*/, int stat, int full, int disabled) {
    void* result = g_origRowCtor(row, stat, full, disabled);
    g_rows.push_back({row, stat, (full & 0xFF) != 0, false});
    return result;
}

static void __fastcall RowDtorDetour(void* row, void* /*edx*/) {
    for (auto it = g_rows.begin(); it != g_rows.end(); ++it) {
        if (it->row == row) {
            g_rows.erase(it);
            break;
        }
    }
    g_origRowDtor(row);
}

// ── The favorites group ────────────────────────────────────────────────

static size_t GroupCount(void* window) {
    auto* first = Field<uint8_t*>(window, kWindowGroups);
    auto* last  = Field<uint8_t*>(window, kWindowGroups + 4);
    return static_cast<size_t>(last - first) / kGroupSize;
}

static const char* GroupName(void* window, size_t index) {
    auto* group = Field<uint8_t*>(window, kWindowGroups) + index * kGroupSize;
    return reinterpret_cast<const AOString*>(group + kGroupName)->c_str();
}

static std::vector<int> GroupStats(void* window, size_t index) {
    auto* list  = Field<uint8_t*>(window, kWindowStatLists) + index * kStatListSize;
    auto* first = Field<int*>(list, 0);
    auto* last  = Field<int*>(list, 4);
    return std::vector<int>(first, last);
}

static bool HasFavoritesGroup(void* window) {
    const size_t count = GroupCount(window);
    return count == kStockGroupCount + 1 &&
           std::strcmp(GroupName(window, count - 1), kGroupKey) == 0;
}

// Stats of the favorites group made by the last InitGroups, for the window
// constructor to find its rows. Empty if no group was added.
static std::vector<int> g_newGroupStats;

static void __fastcall InitGroupsDetour(void* window, void* /*edx*/) {
    g_origInitGroups(window);
    g_newGroupStats.clear();
    if (!FavoritesEnabled()) return;

    if (GroupCount(window) != kStockGroupCount) {
        Log("[skillfav] %u skill groups, expected %u (client update?) — no favorites",
            static_cast<unsigned>(GroupCount(window)), static_cast<unsigned>(kStockGroupCount));
        return;
    }
    if (!EnsureFavoritesLoaded()) Log("[skillfav] no character yet, favorites not loaded");

    // Every skill except the deprecated ones, favorites first, stock order.
    std::vector<int> all;
    for (size_t g = 0; g < kStockGroupCount; ++g) {
        if (std::strcmp(GroupName(window, g), "disabled") == 0) continue;
        for (int stat : GroupStats(window, g))
            if (std::find(all.begin(), all.end(), stat) == all.end()) all.push_back(stat);
    }
    std::stable_partition(all.begin(), all.end(), IsFavorite);

    // Group {String label; String name} built the way InitGroups builds its
    // own: on the stack, copied in by push_back, then destroyed.
    alignas(4) uint8_t group[kGroupSize] = {};
    auto* label = reinterpret_cast<AOString*>(group);
    auto* name  = reinterpret_cast<AOString*>(group + kGroupName);
    *label = AOString::FromShort("");
    *name  = AOString::FromShort("");
    g_stringAssign(label, kGroupLabel);
    g_stringAssign(name, kGroupKey);
    g_groupPushBack(static_cast<uint8_t*>(window) + kWindowGroups, group);
    g_stringDtor(name);
    g_stringDtor(label);

    g_statListsResize(static_cast<uint8_t*>(window) + kWindowStatLists, kStockGroupCount + 1);
    void* list = Field<uint8_t*>(window, kWindowStatLists) + kStockGroupCount * kStatListSize;
    for (int stat : all) g_statPushBack(list, &stat);

    g_newGroupStats = all;
}

// The constructor makes the rows group by group, a compact and a full row
// per stat, so the favorites group's rows are the last 2 * N it makes —
// if Skills.xml has both of the group's views (otherwise it makes none).
static void FindFavoritesRows(void* window, size_t firstRow) {
    const std::vector<int>& stats = g_newGroupStats;
    void* root = Field<void*>(window, kWindowRootView);
    if (stats.empty() || !root) return;
    if (!g_findChild(root, "favorites_view", true) || !g_findChild(root, "favorites_group", true)) {
        Log("[skillfav] Skills.xml has no favorites views — group not shown");
        return;
    }

    const size_t count = stats.size() * 2;
    if (g_rows.size() < firstRow + count) {
        Log("[skillfav] expected %u favorites rows, the window made %u rows",
            static_cast<unsigned>(count), static_cast<unsigned>(g_rows.size() - firstRow));
        return;
    }
    const size_t tail = g_rows.size() - count;
    for (size_t i = 0; i < count; ++i) {
        const RowInfo& info = g_rows[tail + i];
        if (info.stat != stats[i / 2] || info.full != (i % 2 == 1)) {
            Log("[skillfav] favorites rows out of order at %u (stat %d) — group not used",
                static_cast<unsigned>(i), info.stat);
            return;
        }
    }
    for (size_t i = tail; i < g_rows.size(); ++i) g_rows[i].favorites = true;
}

static void* __fastcall WindowCtorDetour(void* window, void* /*edx*/) {
    const size_t firstRow = g_rows.size();
    g_newGroupStats.clear();
    void* result = g_origWindowCtor(window);

    if (!g_newGroupStats.empty()) {
        FindFavoritesRows(window, firstRow);
    } else if (void* root = Field<void*>(window, kWindowRootView)) {
        // Feature off: Skills.xml still has the button, with nothing behind it.
        if (void* button = g_findChild(root, kGroupKey, true)) g_show(button, false, true);
    }
    if (FavoritesEnabled()) {
        for (size_t i = firstRow; i < g_rows.size(); ++i)
            ApplyFavorite(g_rows[i], IsFavorite(g_rows[i].stat));
    }
    g_newGroupStats.clear();
    return result;
}

// Hide the favorites group from the suggested distribution, which goes
// through the groups and would raise each favorite twice as fast.
static void __fastcall SuggestIPDetour(void* window, void* /*edx*/, void* changed) {
    if (!HasFavoritesGroup(window)) {
        g_origSuggestIP(window, changed);
        return;
    }
    Field<uint8_t*>(window, kWindowGroups + 4) -= kGroupSize;
    Field<uint8_t*>(window, kWindowStatLists + 4) -= kStatListSize;
    g_origSuggestIP(window, changed);
    Field<uint8_t*>(window, kWindowGroups + 4) += kGroupSize;
    Field<uint8_t*>(window, kWindowStatLists + 4) += kStatListSize;
}

static void ToggleFavorite(int stat) {
    if (!EnsureFavoritesLoaded()) {
        Log("[skillfav] no character yet, ignoring favorite click");
        return;
    }
    const bool favorite = !IsFavorite(stat);
    if (favorite)
        g_favorites.push_back(stat);
    else
        g_favorites.erase(std::find(g_favorites.begin(), g_favorites.end(), stat));
    SaveFavorites();

    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].stat == stat) ApplyFavorite(g_rows[i], favorite);
    Log("[skillfav] %s stat %d, %u favorite(s)", favorite ? "added" : "removed",
        stat, static_cast<unsigned>(g_favorites.size()));
}

static bool CtrlAltHeld(const void* view) {
    const unsigned q = g_getQualifiers(view);
    return (q & kQualifierCtrl) && (q & kQualifierAlt);
}

static void __fastcall RowClickedDetour(void* window, void* /*edx*/, void* row) {
    if (row && FavoritesEnabled() && CtrlAltHeld(row)) {
        if (const RowInfo* info = FindRow(row)) {
            ToggleFavorite(info->stat);
            return;
        }
    }
    g_origRowClicked(window, row);
}

// ── + / - buttons ──────────────────────────────────────────────────────

static bool ShiftOnly(const void* view) {
    const unsigned q = g_getQualifiers(view);
    return (q & kQualifierShift) && !(q & (kQualifierCtrl | kQualifierAlt));
}

// Pressed adds the first point and fires the click signal (selection,
// details), so the extra points go in before it.
static void __fastcall IncPressedDetour(void* row, void* /*edx*/) {
    if (FavoritesEnabled() && CtrlAltHeld(row)) return;  // the favorite gesture
    if (ShiftStepEnabled() && ShiftOnly(row) && !Field<bool>(row, kRowDisabled)) {
        const int room = Field<int>(row, kRowCap) - Field<int>(row, kRowBase) -
                         Field<int>(row, kRowPending);
        if (room > 1) ChangeDeltaDetour(row, nullptr, (std::min)(kShiftStep - 1, room - 1));
    }
    g_origIncPressed(row);
}

static void __fastcall DecPressedDetour(void* row, void* /*edx*/) {
    if (FavoritesEnabled() && CtrlAltHeld(row)) return;
    if (ShiftStepEnabled() && ShiftOnly(row) && !Field<bool>(row, kRowDisabled)) {
        const int pending = Field<int>(row, kRowPending);
        if (pending > 1) ChangeDeltaDetour(row, nullptr, -(std::min)(kShiftStep - 1, pending - 1));
    }
    g_origDecPressed(row);
}

// Released stops the auto-repeat, goes to the cap / zero if any qualifier
// is held, then fires the click signal again so the details catch up. With
// Shift alone the jump is swallowed and the rest kept.
static void Released(FnThis original, void* row) {
    if (FavoritesEnabled() && CtrlAltHeld(row)) return;  // nothing was pressed
    if (ShiftStepEnabled() && ShiftOnly(row)) {
        g_swallowChangeDelta = true;
        original(row);
        g_swallowChangeDelta = false;
        return;
    }
    original(row);
}

static void __fastcall IncReleasedDetour(void* row, void* /*edx*/) { Released(g_origIncReleased, row); }
static void __fastcall DecReleasedDetour(void* row, void* /*edx*/) { Released(g_origDecReleased, row); }

// ── Skills.xml ─────────────────────────────────────────────────────────

constexpr char kButtonAnchor[] = "<Button name=\"abilities\"";
constexpr char kGroupAnchor[]  = "name=\"disabled_group\"";
constexpr char kButtonXml[] =
    "<Button name=\"favorites\" label=\"Favorites\" layout_borders=\"Rect(5,5,5,5)\""
    " width_group=\"buttons\" width_group_owner=\"parent\"/>\n"
    "            <BorderView view_layout=\"vertical\" name=\"favorites_view\""
    " layout_borders=\"Rect(5,5,5,5)\">\n"
    "            </BorderView>\n"
    "            ";
// Last page of the group selector: pages are picked by group index (11).
constexpr char kGroupXml[] =
    "\n                <View view_layout=\"vertical\" name=\"favorites_group\""
    " layout_borders=\"Rect(5,5,5,5)\">\n"
    "                </View>";

static bool ReadFile(const char* path, std::string& out) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(file, nullptr);
    bool ok = size != INVALID_FILE_SIZE && size <= 1024 * 1024;
    if (ok) {
        out.resize(size);
        DWORD read = 0;
        ok = ::ReadFile(file, &out[0], size, &read, nullptr) && read == size;
    }
    CloseHandle(file);
    return ok;
}

static void PatchSingleSkillsXml(const char* path) {
    std::string xml;
    if (!ReadFile(path, xml)) return;  // this GUI doesn't override Skills.xml
    if (xml.find("\"favorites_view\"") != std::string::npos) return;

    const size_t button = xml.find(kButtonAnchor);
    const size_t group  = xml.find(kGroupAnchor);
    const size_t groupEnd = group == std::string::npos ? group : xml.find("</View>", group);
    if (button == std::string::npos || groupEnd == std::string::npos || groupEnd < button) {
        Log("[skillfav] %s: unknown layout, not patched", path);
        return;
    }
    xml.insert(groupEnd + std::strlen("</View>"), kGroupXml);  // later one first
    xml.insert(button, kButtonXml);

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log("[skillfav] cannot write %s (%lu)", path, GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(file, xml.data(), static_cast<DWORD>(xml.size()), &written, nullptr);
    CloseHandle(file);
    Log("[skillfav] added the favorites group to %s", path);
}

void PatchSkillsXml() {
    ForEachGuiFile("Views\\Skills.xml", PatchSingleSkillsXml);
}

// ── Init ───────────────────────────────────────────────────────────────

template <typename T>
static bool Resolve(T& out, const char* module, const char* name) {
    HMODULE mod = GetModuleHandleA(module);
    out = mod ? reinterpret_cast<T>(GetProcAddress(mod, name)) : nullptr;
    if (!out) Log("[skillfav] %s!%s not found", module, name);
    return out != nullptr;
}

static void* CheckSeh(const SehFunction& fn) {
    const auto gui = reinterpret_cast<uintptr_t>(GetModuleHandleA("GUI.dll"));
    auto* code = static_cast<const uint8_t*>(ResolveRVA("GUI.dll", fn.rva));
    uint32_t table = 0;
    int32_t  rel   = 0;
    if (code) {
        std::memcpy(&table, code + 1, 4);
        std::memcpy(&rel, code + 6, 4);
    }
    if (!code || code[0] != 0xB8 || table != gui + fn.ehTableRVA || code[5] != 0xE8 ||
        reinterpret_cast<uintptr_t>(code) + 10 + rel != gui + kEhPrologRVA) {
        Log("[skillfav] %s at RVA 0x%X doesn't match (client update?)", fn.name, fn.rva);
        return nullptr;
    }
    return const_cast<uint8_t*>(code);
}

static void* CheckPlain(const PlainFunction& fn) {
    void* code = ResolveRVA("GUI.dll", fn.rva);
    if (!code || std::memcmp(code, fn.sig, sizeof(fn.sig)) != 0) {
        Log("[skillfav] %s at RVA 0x%X doesn't match (client update?)", fn.name, fn.rva);
        return nullptr;
    }
    return code;
}

template <typename T>
static bool Hook(void* target, void* detour, T& original, const char* name) {
    void* trampoline = nullptr;
    if (!InstallHook(target, detour, &trampoline)) {
        Log("[skillfav] %s hook failed", name);
        return false;
    }
    original = reinterpret_cast<T>(trampoline);
    return true;
}

bool InitSkillFavorites() {
    bool ok = true;
    ok &= Resolve(g_getQualifiers, "GUI.dll", "?GetQualifiers@View@@QBEIXZ");
    ok &= Resolve(g_show,          "GUI.dll", "?Show@View@@QAEX_N0@Z");
    ok &= Resolve(g_findChild,     "GUI.dll", "?FindChild@View@@QAEPAV1@PBD_N@Z");
    ok &= Resolve(g_getText, "GUI.dll",
        "?GetTextBuffer@TextView_c@@QBEABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
    ok &= Resolve(g_setText, "GUI.dll",
        "?SetText@TextView_c@@QAEXABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    ok &= Resolve(g_stringAssign,  "Utils.dll", "??4String@@QAEAAV0@PBD@Z");
    ok &= Resolve(g_stringDtor,    "Utils.dll", "??1String@@QAE@XZ");
    ok &= Resolve(g_n3GetInstance, "Interfaces.dll", "?GetInstance@N3InterfaceModule_t@@SAPAV1@XZ");
    ok &= Resolve(g_getClientInst, "Interfaces.dll", "?GetClientInst@N3InterfaceModule_t@@QBEIXZ");
    if (!ok) return false;

    if (!BuildFavoritesPath()) {
        Log("[skillfav] can't build AOReloadedSkills.ini path");
        return false;
    }

    // Check everything before hooking anything: the hooks only work together.
    void* windowCtor  = CheckSeh(kWindowCtor);
    void* initGroups  = CheckSeh(kInitGroups);
    void* suggestIP   = CheckSeh(kSuggestIP);
    void* rowClicked  = CheckSeh(kRowClicked);
    void* rowCtor     = CheckSeh(kRowCtor);
    void* rowDtor     = CheckSeh(kRowDtor);
    void* changeDelta = CheckSeh(kChangeDelta);
    void* incPressed  = CheckSeh(kIncPressed);
    void* incReleased = CheckSeh(kIncReleased);
    void* decPressed  = CheckSeh(kDecPressed);
    void* decReleased = CheckSeh(kDecReleased);
    g_groupPushBack   = reinterpret_cast<FnPushBack>(CheckPlain(kGroupPushBack));
    g_statListsResize = reinterpret_cast<FnResize>(CheckPlain(kStatListsResize));
    g_statPushBack    = reinterpret_cast<FnPushBack>(CheckPlain(kStatPushBack));
    if (!windowCtor || !initGroups || !suggestIP || !rowClicked || !rowCtor || !rowDtor ||
        !changeDelta || !incPressed || !incReleased || !decPressed || !decReleased ||
        !g_groupPushBack || !g_statListsResize || !g_statPushBack)
        return false;

    // Row bookkeeping first, so no row can exist without it.
    if (!Hook(rowDtor,     reinterpret_cast<void*>(&RowDtorDetour),     g_origRowDtor,     kRowDtor.name)) return false;
    if (!Hook(rowCtor,     reinterpret_cast<void*>(&RowCtorDetour),     g_origRowCtor,     kRowCtor.name)) return false;
    if (!Hook(changeDelta, reinterpret_cast<void*>(&ChangeDeltaDetour), g_origChangeDelta, kChangeDelta.name)) return false;
    if (!Hook(suggestIP,   reinterpret_cast<void*>(&SuggestIPDetour),   g_origSuggestIP,   kSuggestIP.name)) return false;
    if (!Hook(windowCtor,  reinterpret_cast<void*>(&WindowCtorDetour),  g_origWindowCtor,  kWindowCtor.name)) return false;
    if (!Hook(initGroups,  reinterpret_cast<void*>(&InitGroupsDetour),  g_origInitGroups,  kInitGroups.name)) return false;
    if (!Hook(rowClicked,  reinterpret_cast<void*>(&RowClickedDetour),  g_origRowClicked,  kRowClicked.name)) return false;
    if (!Hook(incPressed,  reinterpret_cast<void*>(&IncPressedDetour),  g_origIncPressed,  kIncPressed.name)) return false;
    if (!Hook(incReleased, reinterpret_cast<void*>(&IncReleasedDetour), g_origIncReleased, kIncReleased.name)) return false;
    if (!Hook(decPressed,  reinterpret_cast<void*>(&DecPressedDetour),  g_origDecPressed,  kDecPressed.name)) return false;
    if (!Hook(decReleased, reinterpret_cast<void*>(&DecReleasedDetour), g_origDecReleased, kDecReleased.name)) return false;

    Log("[skillfav] skills window favorites and shift+click installed (favorites in %s)",
        g_favoritesPath);
    return true;
}

}  // namespace aor
