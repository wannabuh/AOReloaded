#pragma once

// AOReloaded settings system.
//
// Manages persistent settings stored in AOReloaded.ini (next to the exe).
// Each setting is backed by a DValue in the game's registry, so the options
// panel widgets bind to them directly. A detour on SetDValue catches all
// changes and writes them to the .ini immediately.
//
// Init flow (called from DeferredInit):
//   0. IsDebugLogEnabled()   — reads .ini BEFORE LogInit (no game DLLs needed)
//   1. SettingsInit()         — resolve .ini path, load persisted values
//   2. SettingsRegisterAll()  — register DValues with loaded (or default) values
//   2b. PatchOptionsXml()    — inject AOReloaded tab into Root.xml if missing
//   3. SettingsInstallHook()  — detour SetDValue for change persistence
//                               (call after game world is up, so we don't
//                               spam the .ini during the game's own init)

namespace aor {

// Early check: reads AOR_DebugLog from AOReloaded.ini using only Win32 API.
// Safe to call from DLL_PROCESS_ATTACH — does not touch game DLLs.
// Returns false if the key is missing (default: logging off).
bool IsDebugLogEnabled();

// Step 1: Resolve the .ini file path and load any previously saved values
// into an internal table. Does NOT touch DValues or game state.
void SettingsInit();

// Step 2: Register each setting as a DValue via GameAPI::RegisterXxx,
// using the value loaded from .ini (or the compiled-in default).
void SettingsRegisterAll();

// Step 2b: Ensure the AOReloaded tab exists in OptionPanel/Root.xml.
// Reads the file, checks for our ScrollView, injects it if missing.
// Must be called BEFORE the game parses the XML (i.e. before game world).
void PatchOptionsXml();

// Step 3: Install the SetDValue detour. Any subsequent SetDValue call
// whose name matches a registered setting will trigger an .ini write.
// Returns false if the hook couldn't be installed.
bool SettingsInstallHook();

// ── Settings change callback ───────────────────────────────────────────
// Notifies registered listeners when an AOReloaded setting is changed
// (e.g. by an options panel slider).  Called after the new value is
// persisted to .ini.

using SettingChangedCallback = void(*)(const char* name, int newValue);

// Give an Int DValue the min/max an OptionSlider needs (also used for the renderer settings tab).
bool SetDValueMinMax(const char* name, int minVal, int maxVal);

// Register a callback.  Up to 4 callbacks supported.
void RegisterSettingCallback(SettingChangedCallback cb);

}  // namespace aor
