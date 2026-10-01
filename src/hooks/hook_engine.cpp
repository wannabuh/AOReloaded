// hook_engine.cpp — 5-byte inline hook installer for x86.
// See hook_engine.h for design notes.

#include "hooks/hook_engine.h"
#include "core/logging.h"

#include <cstring>

namespace aor {

namespace {

// Known-safe prologues.  Most are exactly 5 bytes, but some require
// copying more bytes to avoid splitting a multi-byte instruction.
struct ProloguePattern {
    uint8_t bytes[5];
    uint8_t mask[5];   // 0xFF = exact match, 0x00 = wildcard
    uint8_t copy_len;  // bytes to copy into trampoline (>= 5)
    const char* name;
};

constexpr ProloguePattern kPrologues[] = {
    // mov edi,edi; push ebp; mov ebp,esp  (hot-patch)
    {{0x8B, 0xFF, 0x55, 0x8B, 0xEC}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 5, "hot-patch"},
    // push ebp; mov ebp,esp; sub esp,imm8  (standard frame + local alloc)
    {{0x55, 0x8B, 0xEC, 0x83, 0xEC}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 5, "frame+sub"},
    // The "frame+push-r" patterns match the first 4 bytes; MatchPrologue
    // decodes the instruction that starts at byte 5 to get the copy length.
    // push ebp; mov ebp,esp; push esi  (frame + save reg)
    {{0x55, 0x8B, 0xEC, 0x56, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, 5, "frame+push-r"},
    // push ebp; mov ebp,esp; push edi
    {{0x55, 0x8B, 0xEC, 0x57, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, 5, "frame+push-r"},
    // push ebp; mov ebp,esp; push ebx
    {{0x55, 0x8B, 0xEC, 0x53, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, 5, "frame+push-r"},
    // push ebp; mov ebp,esp; push ecx (+ any 2nd byte — e.g. push ecx; push ecx)
    {{0x55, 0x8B, 0xEC, 0x51, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, 5, "frame+push-r"},
    // push ebp; mov ebp,esp; push eax
    {{0x55, 0x8B, 0xEC, 0x50, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF, 0x00}, 5, "frame+push-r"},
    // push ebp; mov ebp,esp; mov ecx,[addr32]  (9 bytes: 55 8B EC 8B 0D xx xx xx xx)
    // The MOV ECX,[addr32] uses absolute addressing — safe to relocate.
    // Must copy 9 bytes to include the full 6-byte MOV instruction.
    {{0x55, 0x8B, 0xEC, 0x8B, 0x0D}, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 9, "frame+mov-ecx-abs"},
    // mov eax, imm32 — 5-byte relocatable instruction; used by MSVC as an
    // SEH-prolog entry thunk (sets EH handler address, then jmp/call to
    // _SEH_prolog). Safe to relocate into trampoline since it's IP-independent.
    {{0xB8, 0x00, 0x00, 0x00, 0x00}, {0xFF, 0x00, 0x00, 0x00, 0x00}, 5, "mov-eax-imm32"},
};

// Match result: pattern name + copy length.
struct PrologueMatch {
    const char* name;
    uint8_t copy_len;
};

// Length of a position-independent instruction at `bytes`, or 0 if it isn't
// one of the few forms that follow a "push ebp; mov ebp,esp; push r" prologue.
uint8_t InstructionLength(const uint8_t* bytes) {
    const uint8_t op = bytes[0];
    if (op >= 0x50 && op <= 0x5F) return 1;                // push/pop r32
    if (op == 0x6A) return 2;                              // push imm8
    if (op == 0xFF && bytes[1] == 0x75) return 3;          // push [ebp+disp8]
    if (op == 0x8B) {                                      // mov r32, r/m32
        const uint8_t mod = bytes[1] >> 6;
        const uint8_t rm  = bytes[1] & 7;
        if (mod == 3) return 2;                            // mov r32, r32
        if (mod == 1 && rm != 4) return 3;                 // mov r32, [r32+disp8]
    }
    return 0;
}

// Check if target starts with a known-safe prologue.
PrologueMatch MatchPrologue(const uint8_t* bytes) {
    for (const auto& pat : kPrologues) {
        bool match = true;
        for (int i = 0; i < 5; ++i) {
            if ((bytes[i] & pat.mask[i]) != (pat.bytes[i] & pat.mask[i])) {
                match = false;
                break;
            }
        }
        if (!match) continue;

        // A wildcard 5th byte starts an instruction of its own. Copying just
        // its first byte would leave a split instruction in the trampoline
        // (e.g. "push esi; push [ebp+8]": 55 8B EC 56 FF 75 08), so copy the
        // whole of it, or refuse the hook if its length isn't known.
        if (pat.mask[4] == 0x00) {
            const uint8_t len = InstructionLength(bytes + 4);
            if (len == 0) return { nullptr, 0 };
            return { pat.name, static_cast<uint8_t>(4 + len) };
        }
        return { pat.name, pat.copy_len };
    }
    return { nullptr, 0 };
}

// Encode a 32-bit relative jump: E9 <rel32>.
void WriteJmpRel32(uint8_t* from, const void* to) {
    from[0] = 0xE9;
    auto disp = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(to) -
        (reinterpret_cast<uintptr_t>(from) + 5));
    std::memcpy(from + 1, &disp, 4);
}

}  // namespace

bool InstallHook(void* target, void* detour, void** trampoline_out) {
    if (!target || !detour || !trampoline_out) {
        Log("[hook] null argument");
        return false;
    }

    auto* code = static_cast<uint8_t*>(target);

    // Validate prologue.
    auto match = MatchPrologue(code);
    if (!match.name) {
        Log("[hook] unknown prologue at %p: %02X %02X %02X %02X %02X",
            target, code[0], code[1], code[2], code[3], code[4]);
        return false;
    }

    uint8_t copy_len = match.copy_len;

    // Allocate trampoline: copied bytes + 5-byte jmp back.
    // Allocate 32 for alignment (supports up to 27 copied bytes).
    void* trampoline = VirtualAlloc(
        nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        Log("[hook] VirtualAlloc failed: %lu", GetLastError());
        return false;
    }

    auto* tramp = static_cast<uint8_t*>(trampoline);

    // Copy original prologue into trampoline, then jump to target+copy_len.
    std::memcpy(tramp, code, copy_len);
    WriteJmpRel32(tramp + copy_len, code + copy_len);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 32);

    // Publish trampoline BEFORE patching (see ao-crashfix for rationale).
    *trampoline_out = trampoline;

    // Patch target with jmp to detour.
    DWORD old_protect = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        Log("[hook] VirtualProtect failed: %lu", GetLastError());
        *trampoline_out = nullptr;
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    WriteJmpRel32(code, detour);

    DWORD ignored = 0;
    VirtualProtect(target, 5, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    Log("[hook] installed at %p (%s, %d bytes), trampoline=%p, detour=%p",
        target, match.name, copy_len, trampoline, detour);
    return true;
}

void* ResolveRVA(const char* module_name, uint32_t rva) {
    HMODULE mod = GetModuleHandleA(module_name);
    if (!mod) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mod) + rva);
}

}  // namespace aor
