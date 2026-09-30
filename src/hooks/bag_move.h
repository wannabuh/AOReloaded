#pragma once

// Ctrl+click to move an item from the main inventory into a backpack.
//
// The stock client already handles two related gestures in the inventory's
// item-activation slot (GUI.dll, InventoryViewBase_c, see
// docs/inventory_trade_clicks.md): with a trade or shop window open,
// Ctrl+click adds the item to the trade; clicking an item inside an open
// backpack moves it back to the main inventory. What's missing is the way
// in: this hooks the same slot and, for a Ctrl+click on a main-inventory
// item while no trade is open, calls N3Msg_ContainerAddItem — the call the
// inventory's own drop handler uses — with the most recently opened
// container that is still open. Everything else runs the original code.
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
