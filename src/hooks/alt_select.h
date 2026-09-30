#pragma once

// Alt+click multi-select for inventory, backpack and trade/shop item lists.
//
// Alt+click (either button) on an item toggles it in a selection; Alt+click
// on empty space in the list clears it. The selection is limited to one list
// at a time. Selected items are highlighted with the item's own selected
// state (MultiListViewItem_c::Select); inventory lists don't use that state
// themselves, since their feature flags disable list selection.
//
// When any selected item is then moved by the game — dragged onto a
// backpack, the inventory or a trade/shop window, or Ctrl+/right-clicked —
// the same move is repeated for the rest of the selection. This hooks the
// N3InterfaceModule_t calls every move ends in (N3Msg_ContainerAddItem,
// N3Msg_TradeAddItem, N3Msg_TradeRemoveItem, MoveItemToInventory), so the
// server receives the same requests as dragging each item by hand. Drops on
// the ground and moves to a specific inventory slot are not repeated.
//
// Toggled by the AOR_AltSelect DValue (default: on).

namespace aor {

// Install the list click, list destructor and move hooks.
// Call after SettingsRegisterAll(); GUI.dll and Interfaces.dll are static
// imports of the exe and already loaded.
// Returns true if all hooks were installed.
bool InitAltSelect();

}  // namespace aor
