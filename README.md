# AOReloaded

Client mod framework for Anarchy Online. Injects into the game process via DLL search-order hijacking (`version.dll`) and patches game behavior at runtime through inline hooks and the game's own DValue (CVar) system.

Built for the [Project Rubi-Ka](https://project-rk.com/) private server.
There are no guarantees of functionality nor stability on other Anarchy Online client distributions.

### This fork

This is a fork of [Inorien/AOReloaded](https://github.com/Inorien/AOReloaded) with these additions (details in
[FEATURES.md](FEATURES.md)):

- **Frame rate cap** slider (30-500 FPS) replacing the client's fixed 100 FPS limit.
- **Ctrl+click item moves** between your inventory and the backpack you opened last.
- **Alt+click multi-select** in inventory, backpack and trade/shop lists: move one, the rest follow.
- **Inventory pins**: Ctrl+Alt+click keeps items at the top of inventory and backpack lists.
- **Skills window**: a Favorites group (Ctrl+Alt+click a skill) and Shift+click +/- by 5.
- **Renderer tab** for [ao-vk](https://github.com/wannabuh/ao-vk), a Vulkan replacement for the game's
  renderer with modern lighting, shadows and effects.

Everything is configurable in the **AOReloaded** tab (F10), and each feature can be switched off there.

## Installation

1. Download `version.dll` from the [latest release](../../releases/latest).
2. Drop it into the same folder as `AnarchyOnline.exe` (should be `[game root]\client\`).
3. Launch the game normally.

To uninstall, delete `version.dll` from the client folder. The options tab and the skills window's Favorites group are served from memory while the mod is loaded, so nothing is written into the game's GUI files and no leftover GUI remains. (Builds up to 2026-10 wrote those edits to `OptionPanel/Root.xml` and `Views/Skills.xml` on disk; the first launch of this build restores the stock files automatically.)

#### Removing a leftover custom GUI (older versions)

Versions before 2026-10 wrote the **AOReloaded**/**Renderer** options tabs and the skills window's **Favorites** group directly into the game's GUI XML. Deleting `version.dll` did not remove them, so those files can still carry the edits.

**Easiest:** update `version.dll` to the current build, launch the game once (the login screen is enough), quit it, then delete `version.dll`. The startup repair strips the blocks back out of every file it finds.

**Manual:** if you already deleted `version.dll` or would rather not reinstall it, close the game and delete the blocks yourself with a text editor. (The current build serves the tabs from memory while it is loaded, so edits made while `version.dll` is still present will be re-applied on the next launch — remove `version.dll` first, or just use the easiest route above.)

These edits can be in any of:

- `[client]\cd_image\gui\Default\OptionPanel\Root.xml`
- `[client]\cd_image\gui\Default\Views\Skills.xml`
- the same two files inside any other GUI folder under `[client]\cd_image\gui\`
- the same two files inside a custom GUI under `%LocalAppData%\Funcom\Anarchy Online\<hash>\<account>\Gui\<GUI>\`

In each `Root.xml`, remove the two `<ScrollView>` blocks whose opening tag contains `label="AOReloaded"` and `label="Renderer"`. They are the last two blocks before `</root>`; delete from the `<ScrollView ...>` line through its matching `</ScrollView>` line (the block contains no nested `ScrollView`, so the next `</ScrollView>` is the end). Leave everything else untouched.

In each `Skills.xml`, remove these three pieces:

- the line `<Button name="favorites" label="Favorites" ... width_group_owner="parent"/>`
- the `<BorderView ... name="favorites_view" ...>` … `</BorderView>` pair that follows it
- the `<View ... name="favorites_group" ...>` … `</View>` pair near the end of the group `ViewSelector`

The `AOReloaded.ini`, `AOReloadedSkills.ini`, `AOReloadedPins.ini` and `AOReloaded.log` files in the client folder are harmless and can be kept or deleted; they do not affect the GUI.

#### Linux

From TinkeringIdiot:

- Use `WINEPREFIX=~/.prk wineconfig` and add the override for version.dll to ensure the search-order hijack works correctly

## Features

### WoW-style Camera

Replaces the stock mouse camera with modern MMO controls:

- **Auto-follow** — camera smoothly returns behind your character when you move (configurable lerp speed).
- **RMB-drag align** — right-click drag snaps your character to face the camera direction.
- **Mouse-run** — hold both mouse buttons to run forward with mouse steering. Keyboard forward (W) and mouse-run coexist cleanly.

### Castbars+

- The cast bar that is usually tiny and locked to the top left of the screen can now be relocated and resized.
- The cast bar now shows the nano program name rather than just "Nano program". 
- Cast bars exist in a descending stack frame. To move the frame, open F10 -> AOReloaded -> enable the preview bars. The block that appears can be dragged around. When finished, uncheck the preview bars checkbox. 

**NB**: When placing the preview bars, the actual *start position* - i.e., where all bars will appear and stack downwards from - corresponds with the *top bar*. This is some deep clientside behaviour that may be improved upon in a later release. For now, the bars all move together in a stack.

### Autorun+

- Press autorun key once to start running, press it again to stop.
- You can press autorun while holding down W (or even using the LMB+RMB thingy above) and it will keep running once you release the movement key.
- Pressing move-forward, move-backward or both LMB+RMB buttons will cancel the autorun.

### LargeAddressAware (4 GB Memory)

Automatically patches the executable to use up to 4 GB of virtual memory (instead of the default 2 GB), eliminating crashes from memory fragmentation on large maps. Applied once on first launch; takes effect from the second launch onward.
Start the game once after installing this mod, close it (the login screen is fine even) and you're good to go.

### Numpad Chat Fix

When enabled, pressing numpad keys while chat is focused enters numbers rather than triggering camera/movement actions.

### Options Panel

All features are configurable from an **AOReloaded** tab in the in-game options panel (F10).

Note that LMB+RMB mouse-run requires the WoW-style camera option enabled.

## Planned Features

Non-exhaustive list, in no particular order:

- Chat font size slider
- Finer-grained mouse sensitivity sliders
- Chat timestamps
- Inventory and nanoprograms search and filtering
- Killcounter/session stats (XP/hr, etc)
- Network latency display
- Realtime clock on UI / Session playtime timer
- Bag space counter
- Inventory sorting
- Crash hardening
- Map waypoints + breadcrumb trail
- Vendor sell value display
- Large cursor option
- and more

## Building

Requires Clang-cl, Ninja, and MSVC (for headers/libs). CMake 3.24+.

```bash
cmake --preset release
cmake --build build/release
```

Available presets: `debug`, `release`, `dev` (RelWithDebInfo).

The build produces `version.dll`. The post-build step copies it (and its PDB) into `../client/` automatically if the client directory exists.

### Toolchain

| Component | Version |
|-----------|---------|
| Compiler | Clang-cl (LLVM) |
| Linker | lld-link |
| Generator | Ninja |
| Target | i686-pc-windows-msvc (x86) |
| CRT | Static (`/MT`) |
| Standard | C++20 |

## How It Works

1. Windows loads our `version.dll` from the client directory before the real one in System32.
2. All 16 original `version.dll` exports are forwarded transparently to the system DLL.
3. On load, a deferred init thread waits for the game's DLLs to finish loading, then resolves internal game APIs by their mangled C++ symbol names.
4. Inline hooks (5-byte `jmp rel32`) detour game functions through our code. Trampolines preserve original calling conventions.
5. Custom DValues are registered into the game's global CVar registry, where the options panel XML can bind to them directly.

## License

[MIT](LICENSE)
