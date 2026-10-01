// dllmain.cpp — AOReloaded entry point.
//
// We masquerade as version.dll (DLL search order hijacking). The game
// loads us early, before most game DLLs are mapped. We therefore do
// only safe, minimal work in DLL_PROCESS_ATTACH:
//
//   1. Init logging.
//   2. Suppress thread attach/detach notifications.
//   3. Spawn a deferred init thread that waits for the game's DLLs
//      to be loaded, then resolves the game API and applies mods.
//
// The version.dll forwarding is handled lazily on first export call,
// which is always after DllMain has returned. No LoadLibrary from
// DllMain.

#include "core/logging.h"
#include "core/laa_patch.h"
#include "core/settings.h"
#include "ao/game_api.h"
#include "hooks/input_handler.h"
#include "hooks/camera_hook.h"
#include "hooks/numpad_fix.h"
#include "hooks/timer_bar_drag.h"
#include "hooks/bag_move.h"

#include <windows.h>

namespace {

bool g_initialised = false;

bool IsAnarchyOnlineProcess() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    const wchar_t* filename = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') filename = p + 1;
    }
    return _wcsicmp(filename, L"AnarchyOnline.exe") == 0;
}

// Deferred initialisation — runs on a background thread after the game's
// DLLs are loaded. We poll for Utils.dll since that's where the core
// game API lives.
DWORD WINAPI DeferredInit(LPVOID /*param*/) {
    aor::Log("[init] deferred init thread started");

    // Wait for Utils.dll to appear. The game loads it during startup.
    // Timeout after 30 seconds to avoid hanging forever on broken installs.
    for (int i = 0; i < 300; ++i) {
        if (GetModuleHandleA("Utils.dll")) break;
        Sleep(100);
    }

    if (!GetModuleHandleA("Utils.dll")) {
        aor::Log("[init] Utils.dll never loaded — aborting init");
        return 1;
    }

    aor::Log("[init] Utils.dll detected, resolving game API...");
    if (!aor::GameAPI::Init()) {
        aor::Log("[init] game API init failed (partial resolve)");
    }

    // Register AOReloaded settings as DValues. The settings system loads
    // persisted values from AOReloaded.ini first, then registers DValues
    // with those values (or defaults if .ini doesn't exist yet).
    // Names must be ≤ 15 chars (AOString SSO limit).
    aor::SettingsInit();
    aor::SettingsRegisterAll();

    // Ensure the AOReloaded tab exists in the options panel XML.
    // Must happen before the game parses Root.xml (during world load).
    aor::PatchOptionsXml();

    // Ctrl+click inventory <-> backpack. GUI.dll and Interfaces.dll are static
    // imports of the exe, so they're loaded already; installing before world
    // load means backpacks the client re-opens on login are tracked too.
    if (!aor::InitBagMove()) {
        aor::Log("[init] bag move failed — ctrl+click won't move items to/from backpacks");
    }

    // Wait for game world. 
    aor::Log("[init] waiting for game world...");
    while (!aor::GameAPI::Exists("camera_mode")) {
        Sleep(100);
    }

    aor::Log("[init] game world detected!");

    // Install the SetDValue hook for settings persistence. Done here
    // (after game world init) to avoid intercepting the hundreds of
    // SetDValue calls the game makes during its own startup.
    aor::SettingsInstallHook();

    // Install input handler hooks (GUI.dll callbacks + movement filter).
    if (!aor::InitInputHandler()) {
        aor::Log("[init] input handler failed — running without input mod");
    }

    // Install camera hooks (N3.dll CalcSteering). Depends on input handler.
    if (!aor::InitCameraHooks()) {
        aor::Log("[init] camera hooks failed — running without camera mod");
    }

    // Install numpad text input fix (GUI.dll CheckInput hook).
    if (!aor::InitNumpadFix()) {
        aor::Log("[init] numpad fix failed — numpad keys won't type in chat");
    }

    // Install timer bar drag (GUI.dll CreateTimer hook + mouse filter).
    if (!aor::InitTimerBarDrag()) {
        aor::Log("[init] timer bar drag failed — bars not draggable");
    }

    aor::Log("[init] AOReloaded ready");
    return 0;
}

void OnProcessAttach(HINSTANCE self) {
    if (g_initialised) return;
    g_initialised = true;

    DisableThreadLibraryCalls(self);

    if (!IsAnarchyOnlineProcess()) return;

    // Only create the log file if debug logging is enabled in AOReloaded.ini.
    // IsDebugLogEnabled reads the .ini directly — no game DLLs needed.
    // When disabled, all Log() calls silently no-op.
    if (aor::IsDebugLogEnabled()) {
        aor::LogInit();
    }
    aor::Log("[init] AOReloaded v0.1.0 loading");

    // Patch the exe on disk to enable LargeAddressAware if it isn't
    // already set. Takes effect on the next launch. Only uses Win32 API.
    aor::PatchLargeAddressAware();

    // Spawn the deferred init thread. We can't resolve game APIs here
    // because the game's DLLs aren't loaded yet during DLL_PROCESS_ATTACH.
    HANDLE thread = CreateThread(nullptr, 0, DeferredInit, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        aor::Log("[init] failed to create deferred init thread: %lu",
                 GetLastError());
    }
}

void OnProcessDetach() {
    if (!g_initialised) return;
    aor::Log("[shutdown] AOReloaded unloading");
    aor::LogShutdown();
    g_initialised = false;
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*reserved*/) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: OnProcessAttach(hinstDLL); break;
        case DLL_PROCESS_DETACH: OnProcessDetach(); break;
        default: break;
    }
    return TRUE;
}
