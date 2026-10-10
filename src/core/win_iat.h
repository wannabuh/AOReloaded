#pragma once

// Process-wide import-address-table (IAT) patching.
//
// Replaces every import slot *named* `func` in every currently loaded
// module with `hook`. Matching is by import name, not by the DLL it comes
// from, so it also catches modules that import the function through a
// forwarder (kernelbase / api-ms-win-*).
//
// Callers must resolve and store the original function pointer themselves
// BEFORE calling this, so a thread that runs the hook immediately after it
// is written can always find the real function.

namespace aor {

// Replace all IAT slots importing `func` with `hook`. Idempotent (patching
// a slot that already holds `hook` is harmless). Returns the number of
// slots patched. Safe to call again after more modules have loaded.
int PatchImportsEverywhere(const char* func, void* hook);

}  // namespace aor
