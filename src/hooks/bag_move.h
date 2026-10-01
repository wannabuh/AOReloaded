#pragma once

// Ctrl+click to move items between the main inventory and backpacks.
//
// The stock client handles Ctrl+click in the inventory's item-activation
// slot (GUI.dll, InventoryViewBase_c, see docs/inventory_trade_clicks.md)
// only while a trade or shop window is open: it adds the item to the trade.
// Otherwise a click uses the item, in backpack windows too. This hooks the
// same slot and, for a Ctrl+click while no trade is open:
//   - main inventory item: N3Msg_ContainerAddItem — the call the inventory's
//     own drop handler uses — into the most recently opened container that
//     is still open;
//   - item in an open backpack: MoveItemToInventory, the call a drag back
//     to the inventory ends in.
// Everything else runs the original code. With an Alt+click selection,
// alt_select repeats either move for the rest of the selection.
//
// Open containers are tracked by hooking InventoryGUIModule_c's
// SlotContainerOpened / SlotContainerClosed.
//
// Toggled by the AOR_BagMove DValue (default: on).

namespace aor {

// Install the item-activation and container open/close hooks.
// Call after the game world is up (GUI.dll and Interfaces.dll loaded).
// Returns true if all hooks were installed.
bool InitBagMove();

}  // namespace aor
