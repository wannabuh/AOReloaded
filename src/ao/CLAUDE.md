# AO Client Interop (src/ao/)

Reverse-engineered types and runtime API bindings for interacting with the AO client.

## Key Files

- `types.h` — Struct layouts for AOString (0x18), AOVariant (0x10), AODistributedValue (0x50). All under `#pragma pack(push, 4)` to match MSVC x86 ABI.
- `game_api.h/cpp` — Runtime-resolved function pointers from Utils.dll. Uses MSVC mangled names via GetProcAddress.
- `dvalue_dump.h/cpp` — Walks the global DValue registry's internal std::map red-black tree to enumerate all entries.

## DValue Registry Internals (Utils.dll)

The global registry is an `std::map<String, ValueEntry>` backed by a red-black tree.

**Global addresses (RVAs in Utils.dll):**
- `_Myhead` (sentinel node pointer): RVA `0x2e61c`
- `_Mysize` (element count): RVA `0x2e620`
- Global mutex: RVA `0x2e648` (ACE_Thread_Mutex)

**Tree node layout (verified by hex dump 2026-04-14):**
```
+0x00: Node* _Left
+0x04: Node* _Parent
+0x08: Node* _Right
+0x0C: uint32_t (unknown field — NOT the key)
+0x10: AOString key (0x18 bytes)
+0x28: AOString default_key (0x18 bytes, duplicate)
+0x50: uint32_t variant_type (Variant tag)
+0x54: uint32_t (padding)
+0x58: 8-byte value payload
+0x61: bool has_min
+0x62: bool has_max
+0x68: Variant min_value (0x10 bytes)
+0x78: Variant max_value (0x10 bytes)
+0x88: int32_t category (DValueCategory_e)
+0x8C: vector<DistributedValue_c*> observers
+0xA0: char _Color
+0xA1: char _Isnil (0=valid, 1=sentinel)
```

**DValue categories:**
- 0: Global runtime state (CharacterID, Username)
- 1: Client-wide persistent settings (resolution, font, sound)
- 2: Per-user preferences (mouse, zoom, volume, ESC toggles)
- 3: Per-character UI state (window configs, visibility)
- 4: Transient event signals (camera_mode, module activators)

## Variant Type Tags

| Tag | Type | Storage |
|-----|------|---------|
| 0 | None | — |
| 4 | Int/UInt | 4 bytes at +0x08 |
| 5 | Int64 | 8 bytes at +0x08 |
| 6 | Bool | 1 byte at +0x08 |
| 7 | Float | 4 bytes at +0x08 |
| 8 | Double | 8 bytes at +0x08 |
| 9 | String | heap ptr at +0x08 |
| 10-12 | IRect/IPoint/Message | size at +0x08, heap ptr at +0x0C |
| 16-17 | Rect/Point | size at +0x08, heap ptr at +0x0C |

## Calling Convention Notes

- Static `SetDValue`/`GetDValue`: `__cdecl`
- `GetDValue` returns Variant by value — on MSVC x86, structs >8 bytes use hidden return pointer as first arg
- Instance methods: `__thiscall` (this in ECX)
- All implementations in Utils.dll, imported by GUI.dll and Gamecode.dll
- For inline-hook detours over `__thiscall`, declare the detour as `__fastcall(void* this_ecx, void* edx_unused, ...stack_args)`. ECX is preserved as `this`, EDX is the unused fastcall slot, and remaining stack args follow. Type the trampoline back as the original `__thiscall` so the trampoline's `RET N` cleanup matches the caller's expectations.
- **Long DValue names** (> 15 chars, e.g. `MouseTurnSensitivity`): `AOString::FromShort` only handles SSO (≤ 15 chars). For longer names, use `MakeString()` in `camera_hook.cpp` which constructs a heap-mode `AOString` pointing to a static const — safe because `GetDValue` takes `const AOString&` (read-only).

## N3.dll — Camera System (RE 2026-04-14)

### Resolved exports (mangled name → purpose)

| Mangled name | Purpose |
|---|---|
| `?GetInstance@n3EngineClient_t@@SAPAV1@XZ` | Engine client singleton (also acts as `n3Engine_t`). |
| `?GetActiveCamera@n3EngineClient_t@@QBEPAVn3Camera_t@@XZ` | Returns active `n3Camera_t*`. |
| `?GetClientControlDynel@n3EngineClient_t@@QBEPAVn3VisualDynel_t@@XZ` | Returns the locally controlled player dynel. |
| `?GetGlobalPos@n3Dynel_t@@QBEABVVector3_t@@XZ` | Player position; returns `Vector3_t*` in EAX. |
| `?GetGlobalRot@n3Dynel_t@@QBE?AVQuaternion_t@@XZ` | Player rotation; returns Quaternion by value (hidden ret ptr). |
| `?IsFirstPerson@n3Camera_t@@QBE_NXZ` | Checks current camera mode (1st vs 3rd person). |
| `?ForceUpdatePrefDir@n3Camera_t@@QAEXXZ` | Internally calls `vehicle->ForcedUpdate(false)`. **Does NOT snap behind character** — it just reuses the current camera position as the new preferred direction. |
| `?ForcedUpdate@CameraVehicleFixedThird_t@@UAEX_N@Z` | Virtual; copies position from `+0x58` to `+0xdc`, calls `UpdateHeadingToPos`, then virtual `+0x98`. |
| `?SetRelRot@n3Dynel_t@@QAEXABVQuaternion_t@@@Z` | Writes body rotation (forwards to `Vehicle_t::SetRelRot` on `dynel+0x50`). For the unparented local player, rel == global rot. Used by RMB-align to snap character facing to camera look direction. **Has a synchronous side effect** that recomputes camera targeting from `vehicle+0x1F8` × `vehicle+0x16C` — callers snapping both must write the new heading BEFORE invoking this, or the cascade renders one frame of a stale-heading × new-quat position. |

### View distances (RE 2026-10-09, `hooks/view_distance.cpp`)

- `n3EngineClient_t::GetActiveCamera` = `mov eax,[ecx+0x7c]; ret`. `n3Camera_t +0x174` (float, metres): character view distance; `n3VisualDynel_t::Run` hides characters on ground playfields beyond it (hard cut).
- `FUN_1001f964` (RVA `0x1f964`) copies the `DisplayCharViewDistance` DValue (member at `n3Camera_t +0xb0`) to `+0x174`, replacing values < 5 or > 80 with 70: `0x1f9a6 83 7D F0 50 7E 07` (cmp/jle) is patched to `EB 0B 90 90 90 90`.
- `n3GroundRenderer_t::SlotGroundFQRadiusChanged` passes `DisplayGroundFullQualityRadius` straight to `AnarchyGround_t::SetHQRadius` + `Tesselate` (re-tessellates: game thread only).
- `DistributedValue_c::SetDValue` (Utils.dll `0x25a7`) clamps to the node's min/max (+0x61/+0x62, +0x68/+0x78) before emitting the change signals, on the caller's thread. Stock ranges come from `cd_image/gui/Default/LoginPrefs.xml`.

## Gamecode.dll — Movement dispatcher

### `n3EngineClientAnarchy_t::N3Msg_MovementChanged(MovementAction_e, float, float, bool)`

Mangled: `?N3Msg_MovementChanged@n3EngineClientAnarchy_t@@QAEXW4MovementAction_e@Movement_n@@MM_N@Z` (Gamecode.dll, `__thiscall`). Central dispatcher for all character movement state changes. `n3EngineClientAnarchy_t` derives from `n3EngineClient_t`, so the singleton from `n3EngineClient_t::GetInstance()` can be passed as `this`. The `bool` argument controls server sync.

**MovementAction_e values** (empirically confirmed by hooking the dispatcher and pressing keys):

| Value | Action |
|---|---|
| 1 | StartForward (W) |
| 2 | StopForward (release W) |
| 3 | StartReverse (S) |
| 4 | StopReverse (release S) |
| 5 | TurnLeftStart |
| 6 | TurnLeftStop |
| 7 | TurnRightStart |
| 8 | TurnRightStop |
| 15 (0xf) | JumpStart |
| 22 (0x16) | FullStop (used by `CheckMotionUpdate` heartbeat) |

The RTTI list of `*TransitionAction_t` classes exists at `.rdata 101bf7ec+` but its order does NOT correspond to the enum values; the true mapping above was derived by observation.

## GUI.dll — Signal/Slot System and ActionViewMouseHandler_c

### Signal/Slot Architecture

The game uses a type-safe signal/slot system (`SignalBase_c` / `SlotBase_c` / `SignalTarget_c`). Signals are members on singleton objects (e.g. `WindowController_c`, `GlobalSignals_c`). Slots are heap-allocated objects that store a target pointer + callback function pointer. `SlotObjN_c` templates parameterize the callback arity.

- `WindowController_c::GetInstance()` @ `0x1000b454` — singleton (0x198 bytes). Contains 7 `SignalBase_c` members at `+0x30..+0x44`.
- `GlobalSignals_c::GetInstance()` — via import at `[0x101a70e8]`. Contains signals at various offsets.

### ActionViewMouseHandler_c — Complete RE

**Class layout** (inherits `SignalTarget_c`, total 0x20 bytes):
```
+0x00: vftable* (→ 0x101aeb2c, only inherited destructor)
+0x04: SignalTarget_c slot list
+0x08: int drag_mode         (0=none, 1=LMB_orbit, 2=RMB_steer)
+0x0C: int initiating_button (set on press, copied to +0x08 by BeginDrag)
+0x10: int button_state       (0xFFFFFFFF=none, 1=LMB, 2=RMB)
+0x14: float saved_cursor_x
+0x18: float saved_cursor_y
+0x1C: float accum_drag_dist  (click-vs-drag discriminator)
```

**Signal connections** (wired in constructor @ 0x1002c66b):

| Signal Source | Offset | Callback | Slot Arity | Connector RVA |
|---|---|---|---|---|
| `WindowController_c` | `+0x3C` | `OnMouseDown(Point&, int button, int clickFlags)` | 3 | `0x1002c72e` |
| `WindowController_c` | `+0x40` | `OnMouseUp(Point&, int button)` | 2 | `0x1002c78d` |
| `WindowController_c` | `+0x38` | `OnMouseRelease(Point&, int button)` | 2 | `0x1002c78d` |
| `GlobalSignals_c` | `+0x220` | `OnMouseMove(Point& delta)` | 1 | `0x1002c7ec` |
| `GlobalSignals_c` | `+0x10` | `EndDrag()` | 0 | `0x1002c84b` |
| `GlobalSignals_c` | `+0x08` | `EndDrag()` | 0 | `0x1002c84b` |
| `GlobalSignals_c` | `+0x48` | `EndDrag()` | 0 | `0x1002c84b` |

**Callback prologues and hookability:**

| Callback | RVA | Prologue | RET | Hookable? |
|---|---|---|---|---|
| OnMouseDown | `0x1002c2ee` | `B8 imm32` (SEH) | `RET 0xC` | Yes |
| OnMouseUp | `0x1002c469` | `B8 imm32` (SEH) | `RET 0x8` | Yes |
| OnMouseRelease | `0x1002c14d` | `55 8B EC 56 8B F1` | `RET 0x8` | **No** — 4-byte prolog, 5th byte splits 2-byte instruction |
| OnMouseMove | `0x1002c17b` | `B8 imm32` (SEH) | `RET 0x4` | Yes |
| EndDrag | `0x1002c0e5` | `55 8B EC 51 51` | `RET` | Yes |
| BeginDrag | `0x1002c09b` | (not a signal callback) | — | N/A |

**Other data labels:**
- `ActionViewMouseHandler_c_DRAG_THRESHOLD` @ `0x101aeaf4` — float, click-vs-drag distance threshold used in OnMouseUp
- `ActionViewMouseHandler_c_vftable` @ `0x101aeb2c` — vtable (just SignalTarget_c destructor)

## GUI.dll — Keyboard Input Pipeline

### InputConfig_t — Keyboard Input Dispatcher

Singleton, 0x1D0 bytes. `InputConfig_t::GetInstance()` @ `0x1001b2d5` (`__cdecl`).

**Key fields:**
```
+0x0C: uint8_t  afcm_redirect_mode  (non-zero = hotkey assignment mode, input sent via AFCM)
+0x24: uint8_t  mode_flags[N]       (indexed by mode ID; mode 0 at +0x24, mode 3 at +0x27)
+0x64: uint8_t  dynamic_modes[N]    (indexed by mode ID; mode 3 at +0x67 = text input mode)
+0x120: uint32_t last_key_code      (stored key for repeat detection)
+0x124: uint8_t  key_state[N]       (per-keycode repeat tracking array)
```

**`SetTextInputMode(bool)`** @ `0x10019d21` — writes `this+0x67` (dynamic text input mode flag). Set when a text input widget (chat, search box) gains keyboard focus.

**`CheckMode(int mode)`** @ `0x10019b4c` — returns `this[mode+0x24] || this[mode+0x64]`. For mode 3: checks `this[0x27]` (static) and `this[0x67]` (dynamic text input).

### InputInfo_t — Key Event Structure

```
+0x00: uint32_t key_and_flags
         Bits 0-16:   AO internal key code
         Bit 17:      Ctrl modifier (0x20000)
         Bit 18:      Alt modifier (0x40000)
         Bit 19:      Shift modifier (0x80000)
         Bit 20:      Key-up flag (0x100000)
+0x04: uint8_t  is_repeat (0 = initial press, non-zero = held/repeat)
```

### CheckInput flow (RVA `0x1a4c1`, prologue `55 8B EC 51 53`)

```
CheckInput(InputInfo_t& info):
  1. If afcm_redirect_mode (this+0xc): forward key via AFCM network → return
  2. Call ProcessInput(key, repeat_mode) to match against action bindings
  3. If ProcessInput consumed the key → return (action executed)
  4. If not consumed AND CheckMode(3) → forward to HandleKeyDown/HandleKeyUp
     (dispatches to focused view's KeyDown handler via signal system)
```

**Bug:** Numpad keys are always consumed at step 2 (bound to camera/movement actions) even when text input mode is active. They never reach step 4 or the text input widget.

### ProcessInput (RVA `0x1a31a`)

Iterates the action binding list, matching key codes to commands. Calls `FUN_100180fe` for pre-processing (some bindings have a `+0x70` flag that enables them in text mode). Returns true if a binding matched and executed.

### AO Internal Key Codes — Numpad

Derived from `BrowserModule_c::TranslateKeyCode` (RVA `0x10a751`):

| AO Code | VK Code | Character |
|---------|---------|-----------|
| 0x42 | VK_DIVIDE (0x6F) | `/` |
| 0x43 | VK_MULTIPLY (0x6A) | `*` |
| 0x44 | VK_SUBTRACT (0x6D) | `-` |
| 0x45 | VK_ADD (0x6B) | `+` |
| 0x46 | VK_NUMPAD0 (0x60) | `0` |
| 0x47 | VK_NUMPAD1 (0x61) | `1` |
| 0x48 | VK_NUMPAD2 (0x62) | `2` |
| 0x49 | VK_NUMPAD3 (0x63) | `3` |
| 0x4A | VK_NUMPAD4 (0x64) | `4` |
| 0x4B | VK_NUMPAD5 (0x65) | `5` |
| 0x4C | VK_NUMPAD6 (0x66) | `6` |
| 0x4D | VK_NUMPAD7 (0x67) | `7` |
| 0x4E | VK_NUMPAD8 (0x68) | `8` |
| 0x4F | VK_NUMPAD9 (0x69) | `9` |
| 0x50 | VK_DECIMAL (0x6E) | `.` |

Other notable ranges: F1-F24 = 0x27-0x3E, Letters A-Z = 0x52-0x6B, Digits 0-9 = 0x6C-0x75.

### HandleTextInput (RVA `0x156f92`)

`WindowController_c::HandleTextInput(std::string const&)` — dispatches through signal at focused_view+0x88, then calls vtable+0x50. The `std::string` parameter has the same ABI layout as `AOString` (MSVC 2010).

## GUI.dll — Timer System (TimerSystemModule_t)

### TimerSystemModule_t (0x48 bytes) — singleton

Manages the action timer bars (nano cast, item equip, reload, attack cooldown).

| Field | Offset | Type | Notes |
|-------|--------|------|-------|
| vftable | +0x00 | ptr | SignalTarget_c base |
| EventTimer_c | +0x08 | embedded | Frame timer |
| timer_list | +0x28 | `std::list<TimerBarBase_c*>*` | Heap-allocated doubly-linked list |
| slot_flags | +0x34 | `uint8_t[20]` | 1=free, 0=occupied. Max 20 bars |

**Key RVAs:**

| Function | RVA | Prologue | Notes |
|----------|-----|----------|-------|
| GetInstance (static, lazy) | 0x51d05 | B8 imm32 (SEH) | Static ptr at 0x1818ba |
| CreateTimer | 0x518f0 | B8 imm32 (SEH) | `TimerBar_c* __thiscall(int, Identity_t const&, char const*, uint)` |
| DeleteTimer | 0x517dd | — | Private, takes list iterator. Clears slot_flags[slot_index] at `TSM+0x34`, invokes scalar-deleting dtor at `vftable[1]` with flags=1, then calls `std::list::erase` (FUN_1013205b at `0x13205b` → renamed `std_list_TimerBarPtr_erase`) which unlinks prev/next, `operator_delete(node)`, and decrements size at `list+4` |
| FindNextFreePos | 0x51779 | — | Scans slot_flags for first free slot |
| GetTimer | 0x517a5 | — | Walks list matching Identity_t |
| StartTimerBar | 0x51a27 | B8 imm32 (SEH) | Entry point from game events |
| StopTimerbarMessage | 0x518b0 | — | `void __thiscall(this, Identity_t)` — Identity_t passed by value, flattens to `(int type, int instance)` on the stack. `instance == -1` wildcards. Calls DeleteTimer for every matching bar. Confirmed 2026-04-19 |

### TimerBarBase_c (0x1C bytes) — base class for a single timer bar

| Field | Offset | Type | Notes |
|-------|--------|------|-------|
| vftable | +0x00 | ptr | **Scalar-deleting destructor at vftable[1]** (not [0]; index 0 is a SignalTarget_c-inherited virtual). Call with `flags=1` to run dtor and then `operator delete`. Confirmed via TSM::DeleteTimer decompilation |
| render_window | +0x04 | `RenderWindow_t*` | Visual container sprite |
| power_bar | +0x08 | `PowerBar_t*` | Progress bar widget |
| text_line | +0x0C | `TextLine_t*` | Label text (null if unnamed) |
| identity_type | +0x10 | int | Timer type (1-5) |
| identity_inst | +0x14 | int | Timer instance |
| slot_index | +0x18 | int | Position slot (0-19) |

Constructor at RVA `0x512ae` (SEH prologue).

**TimerBar_c** (0x28 bytes) — extends TimerBarBase_c with 3 zero-init int fields at +0x1C, +0x20, +0x24.

### Position / size formula (TimerBarBase_c ctor)

```
Position: IPoint(0x28, (slot + 2) * 0x14)  →  (40px, (slot+2) * 20px)
```

**Size is NOT 128×16.**  The ctor calls `RenderWindow_t::RenderWindow_t`
with an `IPoint(0x80, 0x10)` (128×16) — but immediately afterwards it
invokes a second `RenderWindow_t::Resize` using
`Sprite_t::GetOriginalSize()` of the fill sprite (GFX `0x1a9`).  The real
stored base at `RenderWindow+0x34/+0x38` is therefore the native
fill-sprite texture dimensions.  Empirically these are **113×10**
(AOReloaded logs `ScaleBar: base=(113,10)`); the first 128×16 is a
discarded scratch value.  Anything that scales relative to "default"
needs to use the actual stored base, not the scratch constants.

### Timer types (from StartTimerBar)

| Type | String | Color | Purpose |
|------|--------|-------|---------|
| 1 | Timer_Attack | 0xaaffaa (green) | Attack cooldown |
| 2 | Timer_Special | 0xaaffff (cyan) | Special ability |
| 3 | Timer_Nano | 0xffaaaa (red) | Nano casting |
| 4 | Timer_Item | 0xaaaaff (blue) | Item equip/unequip |
| 5 | Timer_Reload | 0xffffff (white) | Weapon reload |

### std::list node layout

```
+0x00: void*  next
+0x04: void*  prev
+0x08: TimerBarBase_c*  data
Sentinel = *(TimerSystemModule_t + 0x28)
```

### Sprite_t / RenderWindow_t positioning

- `Sprite_t::Reposition(IPoint const&)` at RVA `0x212a1` — writes x,y at `this+0x20`/`this+0x24`
- `RenderWindow_t::Resize(IPoint const&)` at RVA `0x20938` — writes w,h at `this+0x34`/`this+0x38`, manages internal sprite tile grid

### PowerBar_t (0x74 bytes, inherits HotSpot_t)

| Field | Offset | Type | Notes |
|-------|--------|------|-------|
| fill_sprite | +0x5c | `Sprite_t*` | Bar fill visual |
| bg_sprite | +0x60 | `Sprite_t*` | Background (optional) |
| original_width | +0x6c | int | From GFX texture |
| original_height | +0x70 | int | From GFX texture |

- GFX IDs for timer bars: `0x1a8` (background), `0x1a9` (fill)
- `AdjustPowerLevel(float)` at RVA `0x205de` — sets fill percentage

### TextLine_t (0x60 bytes, inherits SpriteList_t)

- Constructor at RVA `0x223ff`: `TextLine_t(FontID_e, char const*, int layer, IPoint, TextOutputFlags_e, RenderWindow_t*)`
- Text stored at `+0x18` as `std::string` (MSVC 2010 SSO layout)
- `SetDefaultColor(int)` at RVA `0x223ef`

## GUI.dll — N3InterfaceModule_t cached function pointers

Function pointers for N3InterfaceModule_t methods are cached in GUI.dll's data section, populated at runtime by the AFCM module system. Access via `ReadCachedPtr<FnType>(rva)`.

| Function | Data RVA | Signature |
|----------|----------|-----------|
| GetInstance | 0x1a772c | `void* __cdecl()` |
| N3Msg_GetName | 0x1a7724 | `const char* __thiscall(void*, Identity_t const&, Identity_t const&)` |
| N3Msg_GetRarityColor | 0x1a7720 | similar to GetName |
| N3Msg_TemplateIDToDynelID | 0x1a771c | `Identity_t __thiscall(void*, Identity_t const&)` (hidden ret ptr) |
| N3Msg_GetSkill (2-param) | 0x1a7728 | `int __thiscall(void*, Stat_e, int)` — local player |
| N3Msg_GetSkill (4-param) | 0x1a773c | `int __thiscall(void*, Identity_t const&, Stat_e, int, Identity_t const&)` |

**N3Msg_GetName** works with dynel identities (runtime objects), NOT template AOIDs. For nano programs: capture the identity from `CastNanoSpell` (type `0xCF1B`, instance = nano AOID), then pass to `GetName`. Timer identities `{3, 0}` return "NoName".

## Interfaces.dll — N3Msg_CastNanoSpell

`N3Msg_CastNanoSpell(Identity_t const&, Identity_t const&)` — mangled `?N3Msg_CastNanoSpell@N3InterfaceModule_t@@QBEXABVIdentity_t@@0@Z`. Exported from Interfaces.dll. Called by GUI.dll when the player initiates a nano cast.

- Prologue: `55 8B EC 8B 0D` (frame + `MOV ECX,[addr32]`) — requires 9-byte copy in hook engine
- First param: nano identity `{0xCF1B, aoid}`
- Second param: target identity `{0xC350, char_id}`

## ResourceManager.dll — RDB access

Exported functions for synchronous resource loading:

| Function | Mangled | Notes |
|----------|---------|-------|
| `ResourceManager::Get()` | `?Get@ResourceManager@@SAAAV1@XZ` | Static singleton, returns `ResourceManager&` |
| `ResourceManager::GetSync(Identity_t const&, bool)` | `?GetSync@ResourceManager@@QAEPAVDbObject_t@@ABVIdentity_t@@_N@Z` | Loads resource from rdb.db |

RDB identity uses `{TypeID_e, asset_id}` where TypeID_e is the table suffix (e.g., 1000020 for items). The rdb.db is SQLite3 with tables named `rdb_<TypeID>`.

## Hook engine (`src/hooks/hook_engine.cpp`)

Prologue whitelist includes `B8 xx xx xx xx` (`mov eax, imm32`) — the 5-byte MSVC SEH-prolog entry thunk used by functions with try/catch. The instruction is IP-independent and copies cleanly into the trampoline.

The engine now supports **variable-length prologues**: `55 8B EC 8B 0D xx xx xx xx` (`push ebp; mov ebp,esp; mov ecx,[addr32]`) copies 9 bytes into the trampoline. The `MOV ECX,[addr32]` uses absolute addressing so it's safe to relocate. The 5-byte JMP at the original overwrites bytes 0-4; the remaining bytes 5-8 are dead code after the JMP.

**Known limitation:** The `frame+push-r` patterns (`55 8B EC 5x xx`) wildcard the 5th byte (`0x00` mask). If the 5th byte starts a multi-byte instruction (e.g. `8B F1` = `MOV ESI,ECX`), the 5-byte copy splits it — the trampoline executes a garbled instruction and the jump-back lands mid-instruction. Safe only when byte 4 is a complete 1-byte instruction (PUSH r32 range `0x50-0x57`).

### `n3Engine_t` instance (engine timing)

- `n3EngineClient_t::GetInstance()` returns the same singleton as `n3Engine_t::m_pcInstance`.
- `engine + 0x68`: `float dt` (frame delta time, seconds). Used by per-frame camera update.

### `CameraVehicleFixedThird_t` field map (partial)

| Offset | Type | Notes |
|---|---|---|
| `+0x16C` | `Quaternion_t` (4×float, layout `(qx, qy, qz, qw)`) | Character-yaw rotation. Pure Y-axis: `qx=qz=0`. `RecalcOptimalPos` rotates the heading vector at `+0x1F8` by this quaternion when `+0x204` flag is set. |
| `+0x198` | `float` | Preferred camera distance. |
| `+0x1EC..+0x1F4` | `Vector3_t` | Cached optimal camera position (output of `RecalcOptimalPos`). |
| `+0x1F8..+0x200` | `Vector3_t` | **Heading direction**, in pre-rotation (local) space. Steering uses this to compute optimal position. Modifying it is the supported way to redirect the camera. |
| `+0x204` | `uint8_t` | Flag: when non-zero, `RecalcOptimalPos` rotates `+0x1F8` by the quaternion at `+0x16c`. Empirically always 1 in 3rd person gameplay. |
| `+0x208..+0x210` | `Vector3_t` | Steering velocity. Zeroed by `UpdateHeadingToPos`. |

### Heading semantics (important)

The heading at `+0x1F8` is in **character-local space** (pre-rotation). RecalcOptimalPos applies the `+0x16c` quaternion to produce a world-space direction before placing the camera. Implications:

- Local `(0, 0, -1)` → camera ends up directly behind the character regardless of character facing.
- Writing a world-space "behind" direction into `+0x1F8` produces a doubled rotation (or any other character-yaw-dependent error).
- The steering then arrives toward `+0x1EC` — a recalculated point at distance `+0x198` along the rotated heading.

### Hook target — `CameraVehicleFixedThird_t::CalcSteering`

- RVA `0x1f752` in N3.dll
- Signature: `SteeringResult_e __thiscall CalcSteering(Vector3_t& result)` — returns int in EAX, callee cleans 4 stack bytes (`RET 4`).
- Prologue: `55 8B EC 51 56` (`push ebp; mov ebp,esp; push ecx; push esi`) — hookable with the standard 5-byte engine.
- Internally: `RecalcOptimalPos(this)` then either `ZoomSteer` (if zoom velocity at `+0x1B8` ≠ 0) or `SteeringCamArrive` toward the optimal position.

### Reset flag (alternative reset path — not currently used)

Setting bit `0x2000` on `n3Camera_t+0x184` (the camera flags field) makes the next per-frame camera update call the real "behind character" reset routine (the same path used by `COMMAND_RESET_CAMERA` / Numpad 5). The reset uses character facing, not camera position, so it produces a clean snap. This is what we used during early prototyping before switching to the per-frame heading lerp.

## Skills window (`SkillWindow`, GUI.dll)

Used by `src/hooks/skill_favorites.cpp`. All RVAs are GUI.dll; all functions below start with the
SEH prolog `B8 <EH table> E8 <__EH_prolog @ 0x1738a4>`.

Layout comes from `cd_image/gui/<GUI>/Views/Skills.xml` (loaded in the ctor via `"%sViews/Skills.xml"`).

| Function | RVA | Notes |
|---|---|---|
| `SkillWindow::SkillWindow()` | `0xfc18e` | `__thiscall`, RET. Calls InitGroups, then per group finds `<name>_view` (compact list), `<name>_group` (full page) and Button `<name>`; per stat makes two `StatRow`s (compact, full). Group 10 rows are made "disabled". |
| `InitGroups()` | `0xfb596` | `__thiscall`, RET. Fills `+0x7c` (11 groups) and `+0x8c` (stat lists). |
| `DistributeIP(vector<int>* changed)` | `0xfac49` | "Suggested IP distribution" worker, RET 4. Walks groups→stats, finds rows by `"<StatName>_row_ext"`. |
| `OnRowClicked(ButtonBase_c*)` | `0xf97a6` | RET 4. Slot for each StatRow's click signal (+0x148): shows details. |
| `OnGroupButton(ButtonBase_c*)` | `0xf9552` | RET 4. Minimized: toggles `<group>_view` (accordion, open view at `+0x78`). Full: selects page **by group index** in ViewSelector `groupselect`. |
| `OnAccept()` | `0xfa5dd` | For every group stat: FindChild `"<StatName>_row"` (minimized) or `"_row_ext"`, collects base+pending, sends. |
| `SetMinimized` sync | `0xfa1ab` | Copies pending points between the `_row` and `_row_ext` sets: `dst+0x1b0 = src+0x1b0; dst.ChangeDelta(0)`. |
| `StatRow::StatRow(Stat_e, bool full, bool disabled)` | `0xfe956` | RET 0xC. Row name `"<StatName>_row"` / `"_row_ext"`. |
| `StatRow::~StatRow()` | `0xff3f4` | RET (scalar deleting dtor `0xff48a` calls it). |
| `bool StatRow::ChangeDelta(int)` | `0xfde49` | RET 4. Adds to pending, charges the IP difference through signal +0x188 (→ `SkillWindow 0xfa185`, false = can't pay) and backs off one point at a time; `0` = redraw + recompute charge without charging. Abilities: also `N3Msg_SetSkillTmp` when visible. |
| Increase pressed / released | `0xfe261` / `0xfe361` | RET. Pressed: fire click signal, +1, start repeat timer `+0x190` (500 ms, then 50 ms ticks → `0xfe5d7`). Released: stop timer; **any qualifier** → jump to cap; fire click signal. |
| Decrease pressed / released | `0xfe0ca` / `0xfe1b4` | Same with timer `+0x194`; qualifier → back to zero. |

`SkillWindow` fields: `+0x74` root View*, `+0x78` open compact view, `+0x7c` `vector<Group>`
(stride 0x38: String label, String name at +0x1c), `+0x8c` `vector<vector<Stat_e>>` (stride 0x10),
`+0xac` remaining IP, `+0xb8` bool minimized. Vectors here are `{first, last, end, alloc}`.
Helpers: `vector<Group>::push_back` `0xff7cc`, `vector<vector<int>>::resize` `0xff558`,
`vector<int>::push_back` `0x64bee` (all `__thiscall`, RET 4).

`StatRow` (derives from `ButtonBase_c`): `+0x198` TextView_c* name label (text = stat name, only
its colour changes later), `+0x19c`/`+0x1a0` +/- Button_c (signals +0x74 pressed, +0x78 released),
`+0x1a4` stat, `+0x1a8` buffed, `+0x1ac` base, `+0x1b0` pending points, `+0x1b4` cap,
`+0x1b8` IP charged for the pending points, `+0x1bc` value label, `+0x1c4` full-view flag,
`+0x1cc` disabled.
