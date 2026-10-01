#pragma once

// Pinned inventory items: in list mode, pinned items stay at the top of an
// inventory or backpack list whatever column it is sorted by, in the order
// they were pinned.
//
// Ctrl+Alt+click an item to pin or unpin it (alt_select's list mouse-up hook
// forwards the click here). Sorting goes through the item's virtual
// Compare(other, column) — both the full sort on a header click and the
// sorted insertion of a new item — so this replaces that one vtable slot of
// InventoryListViewItem_c with a wrapper that orders pinned items first and
// leaves every other comparison to the original. Grid mode is untouched.
//
// Pins are per character, keyed by the item's identity (its slot) plus its
// name, and saved in AOReloadedPins.ini next to the exe. An item that moves
// to another slot is no longer pinned.
//
// Toggled by the AOR_InvPins DValue (default: on).

namespace aor {

// Patch the InventoryListViewItem_c compare slot.
// Call after SettingsRegisterAll(); GUI.dll is a static import of the exe.
// Returns true if pinning is available.
bool InitInventoryPins();

// Pin or unpin `item` in `list` (Ctrl+Alt+click). Returns false if the item
// isn't an inventory list item or pinning is off, so the caller can carry
// on with its own handling.
bool TogglePinFromClick(void* list, void* item);

}  // namespace aor
