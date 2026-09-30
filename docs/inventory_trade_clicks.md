# Inventory ↔ trade/shop clicks (static analysis, PRK client 0.7.2)

Goal: ctrl+click to move an item between the inventory and an open shop terminal or trade
window instead of dragging it. Findings below come from reading `GUI.dll` statically
(addresses are its preferred ImageBase `0x10000000`; the DLL is relocated at runtime).
**Not yet verified in game.**

## Summary

The stock client appears to support this already, gated by the `UseNewMouseFunc` DValue
(F10 checkbox `#SwitchMouseFunctio`, default `true` in `LoginPrefs.xml`):

| Where | Gesture | Call | Effect |
|---|---|---|---|
| Inventory / backpack item, trade window open | Ctrl + left click | `N3Msg_TradeAddItem(trade+0x1d0, item)` | offer item |
| Own offer list in the trade window | left click | `N3Msg_TradeRemoveItem(trade+0x1d0, item)` | take it back |
| Shop / other party item list | left click | `N3Msg_TradeAddItem(trade+0x1c8, item)` | select to buy |
| "Buying" list | left click | `N3Msg_TradeRemoveItem(trade+0x1c8, item)` | unselect |

All calls go through `N3InterfaceModule_t::GetInstance()` (Interfaces.dll), the same path as
dropping a dragged item, so the server sees identical requests.

## Qualifier bits (`WindowController_c::GetQualifiers`, `0x10156b17`)

From `GetAsyncKeyState`: `VK_LSHIFT 0x1`, `VK_RSHIFT 0x2`, `VK_LCONTROL 0x4`,
`VK_RCONTROL 0x8`, `VK_LMENU 0x10`, `VK_RMENU 0x20`; generic `VK_SHIFT` sets `0x3`,
`VK_CONTROL` sets `0xC`. `BrowserModule_c::TranslateQualifiers` maps `0x3/0xC/0x30` to
Awesomium's Shift/Ctrl/Alt (1/2/4), which confirms the meaning.
`View::GetQualifiers` (`0x1014a6d1`) forwards to the WindowController.

## Click path

1. `MultiListView_c::MouseUp` (`0x10133c28`) fires the list's signal at `+0x134`
   (`MouseDown` fires `+0x130`).
2. The inventory list view (derived from `MultiListView_c`; its constructor around
   `0x10041b01` connects its own base signals) handles `+0x134` in `0x10040a17`
   `(?, MultiListViewItem_c* item, int button, Message* drag)`:
   - `drag != null` → signal `+0x2dc` (drop).
   - otherwise requires the view flag at `+0x319` (set to 1 via `0x10040844` by each
     inventory window when it builds its list) and `UseNewMouseFunc`:
     - button 2 → signal `+0x2e8` (context menu).
     - button 1 with `qualifiers & 0x3` (Shift) → signal `+0x2e4`.
     - button 1 otherwise, if `(byte at +0x150) & 0xC0 == 0` → signal `+0x2e0`.
3. `InventoryViewBase_c` (constructor near `0x100cc206`) connects the child list's
   signals `+0x2d8/+0x2dc/+0x2e0/+0x2e4/+0x2e8` to handlers; `+0x2e0` → `0x100ca1e7`:

   ```cpp
   if (item->check_1003ca2f()) return;
   if (InventoryGUIModule_c::s_pcInstance && module->tradeView /* +0x758 */
       && this->GetQualifiers() & 0xC /* Ctrl */) {
       auto* tv = dynamic_cast<TradeView_c*>(module->tradeView);
       N3InterfaceModule_t::GetInstance()->N3Msg_TradeAddItem(tv+0x1d0, Identity(item+0x20));
       return;
   }
   if (this+0x14c && this+0x140 != 0xDEAD)
       N3InterfaceModule_t::GetInstance()->N3Msg_UseItem(item, false);
   else
       N3InterfaceModule_t::GetInstance()->MoveItemToInventory(item);
   ```

4. `TradeView_c` connects signal `+0x2e0` of the lists in its sub-views:
   - `+0x1e4` list → `0x100df67e`: `N3Msg_TradeRemoveItem(this+0x1d0, item)`
   - `+0x1e0` list → `0x100df6b4`: `N3Msg_TradeRemoveItem(this+0x1c8, item)`, refresh `+0x1e8`
   - `+0x1e8` list → `0x100df6f9`: `N3Msg_TradeAddItem(this+0x1c8, item)`, refresh `+0x1e8`

## Relevant imports (GUI.dll IAT)

| Slot | Function |
|---|---|
| `0x101a772c` | `N3InterfaceModule_t::GetInstance()` |
| `0x101a78e4` | `N3InterfaceModule_t::N3Msg_TradeAddItem(Identity_t const&, Identity_t const&)` |
| `0x101a7a28` | `N3InterfaceModule_t::N3Msg_TradeRemoveItem(Identity_t const&, Identity_t const&)` |
| `0x101a78e0` | `N3InterfaceModule_t::MoveItemToInventory(Identity_t const&)` |
| `0x101a75f0` | `N3InterfaceModule_t::N3Msg_UseItem(Identity_t const&, bool)` |
| `0x101a83b4` | `Variant::operator Identity_t()` (item identity lives in a Variant at `item+0x20`) |

## Open questions (need an in-game test)

- Does Ctrl+click from the inventory work for **shop terminals**, or only player trades?
  Both go through `InventoryGUIModule_c::GetTradeView()` / `SlotStartTrade(TradeType_e, …)`.
- What `(byte at +0x150) & 0xC0` and `check_1003ca2f` reject.
- Whether button 1 is really the left button here (AOReloaded's input handler uses 1 = LMB).
