# AOReloaded — Features

Client mod for Anarchy Online. Drop `version.dll` into your client directory and go.

## Installation

1. Copy `version.dll` into the same folder as `AnarchyOnline.exe`
2. Launch the game normally
3. Check `AOReloaded.log` in the client folder to confirm it loaded

## Features

### WoW-style Camera Auto-Follow
In 3rd person, the camera smoothly returns to a position behind your character whenever you move (and you're not holding LMB). LMB-drag still orbits the camera freely; on release it stays put until you start moving again. The "behind" position is based on which way your character is facing, so walking backwards or strafing won't flip the camera around.

Additionally, any **right-click drag** realigns your character to face the direction the camera is currently looking and snaps the camera to directly behind — so after orbiting with LMB, your next RMB-drag starts from a clean "behind" view instead of preserving the orbit offset.

**Both mouse buttons held** makes your character run forward (WoW-style "mouse-walk"). You can press the buttons in either order; the moment both are held, forward motion starts. Releasing either button falls back to whatever the remaining button does (LMB → orbit, RMB → drag-turn, neither → idle). RMB drag behavior while both are held continues to rotate the character normally.

Forward movement from mouse buttons and the keyboard move-forward key (W / whatever else) coexist cleanly: if you hold both mouse buttons and also press your forward key, releasing either input source does not interrupt the other. You can seamlessly transition between keyboard-driven and mouse-driven forward movement. Strafe and turn keys also work naturally while both mouse buttons are held.

**Config:** Options panel (F10) → **AOReloaded** tab → **Camera**:
- "WoW-style camera (auto-recenter after LMB drag)" — master toggle for the camera system (yaw follow, RMB-align). Lerp speed controlled by the speed slider (default 2; higher = snappier follow).
  - "LMB+RMB mouse-run" — separately toggleable. When off, pressing both mouse buttons does nothing extra (stock behavior). When on, holding both buttons runs forward with mouse steering. Enabled by default, requires WoW-style camera enabled to use.

### LargeAddressAware (4 GB Memory)
On first launch, AOReloaded patches the `AnarchyOnline.exe` on disk to enable the LargeAddressAware flag, which allows the 32-bit client to use up to 4 GB of virtual memory instead of the default 2 GB. This eliminates crashes caused by memory fragmentation when loading large maps or playing for extended sessions. The patch is permanent and takes effect from the second launch onward.

**Config:** None — always on, applied automatically. Start the the game once after installing this mod, close it (the login screen is fine even) and you're good to go.

### Numpad Keys in Chat Fix
When a text input (e.g. chat) has focus, prevents numpad keys from triggering camera/movement actions and instead will write text to text input. Only active when a text input has focus; numpad keys work as normal action bindings during regular gameplay.

**Config:** Options panel (F10) → **AOReloaded** tab → **Input** → "Numpad keys type in chat". Enabled by default.

### Enhanced Timer Bars
The action timer bars (nano casting, item equip/unequip, reload, attack cooldown) have been enhanced:

- **Configurable position and size** — set X/Y position, width, and height via sliders. Positions persist across sessions. Bars stack vertically with a 4-pixel gap regardless of height.
- **Nano program name** — when casting a nano, the bar displays the actual nano program name (e.g. "Balanced Striker") instead of the generic "Nano program" label.
- **Preview & drag-to-position mode** — tick the "Show preview bars" checkbox to spawn five static dummy bars (one per in-game type: Attack, Special, Nano, Item, Reload) at your configured position and size. While this mode is on you can click and drag any bar to reposition the whole group, and the width/height sliders update all five previews live. Untick the checkbox when you're done: preview bars disappear, real bars go back to being purely informational, and clicks on them pass straight through to the game. Labels double as a colour legend so you can see which colour maps to which bar type.

**Config:** Options panel (F10) → **AOReloaded** tab → **Timer Bars**:
- X/Y position sliders (default: 40, 40). Can also be set by dragging while preview mode is on.
- Width slider (default: 113 — native fill-sprite width; pixel-perfect at this value)
- Height slider (default: 10 — native fill-sprite height; pixel-perfect at this value)
- "Show preview bars" checkbox — toggles the five dummy bars AND enables drag-to-reposition. Disabled by default.

### Skills Window: Favorites and Shift+Click
- **Favorites** — a new **Favorites** group at the top of the skills window, above Abilities. Ctrl+Alt+click a skill in any group to add it to your favorites (its name gets a "* " in front); Ctrl+Alt+click it again to remove it. Raise and lower skills in Favorites exactly like in the other groups — points added there show up in the skill's own group too, and IP is only spent once. Favorites are saved per character in `AOReloadedSkills.ini` next to the game exe.
- **Shift+click** on a skill's **+** or **-** changes it by 5 instead of 1 (less if you hit the skill cap or run out of IP). Ctrl+click still goes all the way to the cap (or back to zero) like before.

**Config:** Options panel (F10) → **AOReloaded** tab → **Input** → "Ctrl+Alt+click a skill to add it to the Favorites group" and "Shift+click on a skill's + or - changes it by 5". Both enabled by default.

### Frame Rate Cap
The client normally limits itself to 100 FPS. A slider sets the limit anywhere from 30 to 500 FPS; it changes the game's own frame limiter, so it applies immediately.

**Config:** Options panel (F10) → **AOReloaded** tab → **Performance** → frame rate cap (default 100, the stock value).

### Ctrl+Click Item Moves (Backpacks)
- **Ctrl+click** an item in your main inventory to move it into the backpack you opened last.
- **Ctrl+click** an item in an open backpack to move it back to your inventory.

The moves go through the game's own drag-and-drop path, so the server sees exactly what it would for a manual drag. (Selling with a shop open is the stock Ctrl+right-click.)

**Config:** Options panel (F10) → **AOReloaded** tab → **Input**. Enabled by default.

### Alt+Click Multi-Select
**Alt+click** items in an inventory, backpack or trade/shop list to select several (Alt+click empty space clears the selection). Then move any one of them the usual way - drag it onto a backpack, the inventory or a trade/shop window, or Ctrl+/right-click it - and the same move is repeated for the rest of the selection, as if you had moved each by hand. Drops on the ground and moves to a specific inventory slot are not repeated.

**Config:** Options panel (F10) → **AOReloaded** tab → **Input**. Enabled by default.

### Inventory Pins
**Ctrl+Alt+click** an item (or a stack) in an inventory or backpack list to pin it: pinned items stay at the top of the list, in the order you pinned them, whatever column the list is sorted by. Pinned rows show "* " before the name. Ctrl+Alt+click again to unpin. Pins are saved per character in `AOReloadedPins.ini` next to the game exe; an item moved to another slot is no longer pinned. (List mode only; grid mode is unchanged.)

**Config:** Options panel (F10) → **AOReloaded** tab → **Input**. Enabled by default.

### Renderer Tab (randy-vk)
With [randy-vk](https://github.com/wannabuh/randy-vk) installed (a Vulkan replacement for the game's renderer), the options panel gets a **Renderer** tab with all of its settings: every graphics feature as an on/off checkbox at the top, its sliders and choices (shadow resolutions, anisotropy) below, grouped by feature. Changes apply immediately and are saved by randy-vk in `randy-vk.ini`. Without randy-vk the tab simply doesn't appear.

**Config:** none - present whenever randy-vk is installed.

<!-- 
Template for adding features:

### Feature Name
Brief description of what it does from a player's perspective.

**Config:** How to configure it (options panel, /command, ini file, or "none — always on").

-->

## Options Panel

AOReloaded adds an **AOReloaded** tab to the in-game options panel (F10). Mod settings will appear here as features are added.
