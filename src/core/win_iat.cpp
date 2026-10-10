// win_iat.cpp — process-wide IAT hooking. See win_iat.h.

#include "core/win_iat.h"
#include "core/logging.h"

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>

namespace aor {
namespace {

// Is this module loaded from the Windows directory (kernel32, kernelbase,
// msvcrt, ...)? Those must never be patched: e.g. kernel32.dll imports
// CreateFileW from kernelbase.dll and re-exports it as a thunk, so patching
// that import would make our "real" pointer jump back into our detour.
bool IsSystemModule(const wchar_t* path) {
    wchar_t windowsDir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryW(windowsDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    const size_t len = wcslen(windowsDir);
    if (_wcsnicmp(path, windowsDir, len) != 0) return false;
    return path[len] == L'\\' || path[len] == L'\0';
}

// Patch one module's imports of `func`. Returns the number of slots patched.
int PatchModule(HMODULE module, const char* func, void* hook) {
    if (!module) return 0;
    auto* base = reinterpret_cast<uint8_t*>(module);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return 0;

    int patched = 0;
    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name != 0; ++imp) {
        // Prefer the (unmodified) name table; fall back to the IAT itself.
        const DWORD nameRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + nameRva);
        auto* iat   = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);

        for (; names->u1.AddressOfData != 0; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (_stricmp(reinterpret_cast<const char*>(byName->Name), func) != 0)
                continue;

            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                continue;
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            ++patched;
        }
    }
    return patched;
}

}  // namespace

int PatchImportsEverywhere(const char* func, void* hook) {
    if (!func || !hook) return 0;

    // Never patch our own module's imports: our file helpers must call the
    // real API so the overlay can read/write GUI files directly.
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&PatchImportsEverywhere), &self);

    int modules = 0;
    int total = 0;
    int skipped = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 entry = {};
        entry.dwSize = sizeof(entry);
        if (Module32First(snap, &entry)) {
            do {
                if (entry.hModule == self) continue;

                wchar_t path[MAX_PATH] = {};
                if (GetModuleFileNameW(entry.hModule, path, MAX_PATH) == 0 ||
                    IsSystemModule(path)) {
                    ++skipped;
                    continue;
                }

                const int n = PatchModule(entry.hModule, func, hook);
                if (n > 0) { ++modules; total += n; }
            } while (Module32Next(snap, &entry));
        }
        CloseHandle(snap);
    }

    Log("[overlay] patched imports of %s: %d slot(s) in %d game module(s), %d system skipped",
        func, total, modules, skipped);
    return total;
}

}  // namespace aor
