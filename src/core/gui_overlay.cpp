// gui_overlay.cpp — serve patched GUI XML from memory. See gui_overlay.h.
//
// Interception point: CreateFileW / CreateFileA, hooked through every loaded
// module's IAT (win_iat.cpp). The client opens OptionPanel/Root.xml and
// Views/Skills.xml through these (GUI.dll imports CreateFileW directly and
// also opens files via ACE.dll, which imports both), so an IAT hook catches
// them without depending on the game's own file-loading code. Because the
// CRT's own imports are patched too, a file read via fopen/fread is covered
// as well.
//
// When a registered path is opened we read the real file, run the patcher,
// and hand back a transient (FILE_FLAG_DELETE_ON_CLOSE) copy of the result.
// The copy is what the game reads; the install stays untouched and nothing
// is left behind when the process exits.
//
// The detour runs for every file open in the process, so its no-match path
// must be allocation-free: the tail of the path is compared against the
// registered suffixes in place (no std::string, no conversion).

#include "core/gui_overlay.h"

#include "core/logging.h"
#include "core/win_iat.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace aor::overlay {
namespace {

struct Rule {
    std::string suffix;  // normalized: lowercase, backslashes
    Patcher     fn;
};

constexpr int kMaxRules = 8;
Rule g_rules[kMaxRules];
int  g_ruleCount = 0;

using FnCreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
using FnCreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);

FnCreateFileW g_realW = nullptr;
FnCreateFileA g_realA = nullptr;

// Re-entrancy guard: a hooked module may open a file while we are inside the
// detour. Those calls go straight to the real API.
thread_local int g_depth = 0;

struct ReentryGuard {
    ReentryGuard() { ++g_depth; }
    ~ReentryGuard() { --g_depth; }
};

char FoldAscii(char c) {
    if (c == '/') return '\\';
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

wchar_t FoldAscii(wchar_t c) {
    if (c == L'/') return L'\\';
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// True if `path` ends with `suffix` (ASCII, lowercase, backslashes) at a
// path-component boundary.
bool EndsWithA(const char* path, size_t len, const std::string& suffix) {
    const size_t slen = suffix.size();
    if (len < slen) return false;
    const size_t base = len - slen;
    for (size_t i = 0; i < slen; ++i) {
        if (FoldAscii(path[base + i]) != static_cast<char>(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return base == 0 || path[base - 1] == '\\' || path[base - 1] == '/';
}

bool EndsWithW(const wchar_t* path, size_t len, const std::string& suffix) {
    const size_t slen = suffix.size();
    if (len < slen) return false;
    const size_t base = len - slen;
    for (size_t i = 0; i < slen; ++i) {
        if (FoldAscii(path[base + i]) !=
            static_cast<wchar_t>(static_cast<unsigned char>(suffix[i])))
            return false;
    }
    return base == 0 || path[base - 1] == L'\\' || path[base - 1] == L'/';
}

Patcher MatchPatcher(const char* path, size_t len) {
    for (int i = 0; i < g_ruleCount; ++i)
        if (EndsWithA(path, len, g_rules[i].suffix)) return g_rules[i].fn;
    return nullptr;
}

Patcher MatchPatcher(const wchar_t* path, size_t len) {
    for (int i = 0; i < g_ruleCount; ++i)
        if (EndsWithW(path, len, g_rules[i].suffix)) return g_rules[i].fn;
    return nullptr;
}

bool ReadAll(HANDLE file, std::string& out) {
    const DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size > 4u * 1024 * 1024) return false;
    out.resize(size);
    if (size == 0) return true;
    DWORD read = 0;
    return ReadFile(file, &out[0], size, &read, nullptr) && read == size;
}

// Write `data` to a temp file the OS deletes when its last handle closes.
HANDLE CreateOverlayFile(const std::string& data) {
    wchar_t dir[MAX_PATH] = {};
    const DWORD len = GetTempPathW(MAX_PATH, dir);
    if (len == 0 || len >= MAX_PATH) return INVALID_HANDLE_VALUE;

    static volatile LONG counter = 0;
    const LONG id = InterlockedIncrement(&counter);

    wchar_t path[MAX_PATH] = {};
    if (swprintf(path, MAX_PATH, L"%sAOReloaded-%lu-%ld.tmp",
                 dir, GetCurrentProcessId(), id) < 0)
        return INVALID_HANDLE_VALUE;

    HANDLE file = g_realW(path, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log("[overlay] temp file create failed: %lu", GetLastError());
        return file;
    }

    if (!data.empty()) {
        DWORD written = 0;
        if (!WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) ||
            written != data.size()) {
            CloseHandle(file);
            return INVALID_HANDLE_VALUE;
        }
    }
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    return file;
}

// `source` is an open read handle to the target file. Returns an overlay
// handle, or INVALID_HANDLE_VALUE if the file should be served unchanged.
HANDLE TryOverlay(Patcher patcher, const char* displayName, HANDLE source) {
    ReentryGuard guard;

    std::string in;
    const bool read = ReadAll(source, in);
    CloseHandle(source);
    if (!read) return INVALID_HANDLE_VALUE;

    std::string out;
    if (!patcher(in, out)) return INVALID_HANDLE_VALUE;

    HANDLE overlay = CreateOverlayFile(out);
    if (overlay != INVALID_HANDLE_VALUE) {
        Log("[overlay] serving %s from memory (%u -> %u bytes)",
            displayName, static_cast<unsigned>(in.size()), static_cast<unsigned>(out.size()));
    }
    return overlay;
}

HANDLE WINAPI CreateFileW_Detour(LPCWSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES sa, DWORD creation,
                                 DWORD flags, HANDLE tmpl) {
    if (g_depth > 0 || !name || !g_realW)
        return g_realW(name, access, share, sa, creation, flags, tmpl);

    Patcher patcher = MatchPatcher(name, wcslen(name));
    if (!patcher)
        return g_realW(name, access, share, sa, creation, flags, tmpl);

    HANDLE source = g_realW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source != INVALID_HANDLE_VALUE) {
        char display[MAX_PATH];
        const int n = WideCharToMultiByte(CP_ACP, 0, name, -1, display, MAX_PATH, nullptr, nullptr);
        HANDLE overlay = TryOverlay(patcher, n ? display : "<overlay>", source);
        if (overlay != INVALID_HANDLE_VALUE) return overlay;
    }
    return g_realW(name, access, share, sa, creation, flags, tmpl);
}

HANDLE WINAPI CreateFileA_Detour(LPCSTR name, DWORD access, DWORD share,
                                 LPSECURITY_ATTRIBUTES sa, DWORD creation,
                                 DWORD flags, HANDLE tmpl) {
    if (g_depth > 0 || !name || !g_realA)
        return g_realA(name, access, share, sa, creation, flags, tmpl);

    Patcher patcher = MatchPatcher(name, strlen(name));
    if (!patcher)
        return g_realA(name, access, share, sa, creation, flags, tmpl);

    HANDLE source = g_realA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source != INVALID_HANDLE_VALUE) {
        HANDLE overlay = TryOverlay(patcher, name, source);
        if (overlay != INVALID_HANDLE_VALUE) return overlay;
    }
    return g_realA(name, access, share, sa, creation, flags, tmpl);
}

}  // namespace

void RegisterPatcher(const char* relPath, Patcher fn) {
    if (!relPath || !fn || g_ruleCount >= kMaxRules) return;
    Rule& rule = g_rules[g_ruleCount++];
    rule.fn = fn;
    for (const char* p = relPath; *p; ++p) rule.suffix.push_back(FoldAscii(*p));
}

void Init() {
    if (!g_realW) {
        // Resolve from kernelbase when present: kernel32 re-exports these as
        // thunks through its own import slots, so its address would be the
        // thunk rather than the implementation.
        HMODULE kernelbase = GetModuleHandleA("kernelbase.dll");
        HMODULE source = kernelbase ? kernelbase : GetModuleHandleA("kernel32.dll");
        g_realW = source
            ? reinterpret_cast<FnCreateFileW>(GetProcAddress(source, "CreateFileW"))
            : nullptr;
        g_realA = source
            ? reinterpret_cast<FnCreateFileA>(GetProcAddress(source, "CreateFileA"))
            : nullptr;
        if (!g_realW || !g_realA)
            Log("[overlay] CreateFileW/A not resolved — GUI overlay disabled");
        else
            Log("[overlay] real CreateFileW=%p CreateFileA=%p", reinterpret_cast<void*>(g_realW),
                reinterpret_cast<void*>(g_realA));
    }
    if (g_realW)
        PatchImportsEverywhere("CreateFileW", reinterpret_cast<void*>(&CreateFileW_Detour));
    if (g_realA)
        PatchImportsEverywhere("CreateFileA", reinterpret_cast<void*>(&CreateFileA_Detour));
}

}  // namespace aor::overlay
