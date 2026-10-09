// camera_hook.cpp — Per-frame camera behaviors via CalcSteering hook.
//
// Hooks CameraVehicleFixedThird_t::CalcSteering (N3.dll) for:
//   - Yaw follow during movement (camera swings behind character)
//   - RMB-align (snap character facing to camera direction)
//   - Continuous char-align during BOTH_HELD
//   - Unified forward-movement evaluation (delegated to input_handler)
//
// Reads InputState from input_handler.h — does not manage mouse state.

#include "hooks/camera_hook.h"
#include "hooks/input_handler.h"
#include "hooks/hook_engine.h"
#include "hooks/view_distance.h"
#include "ao/game_api.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <cmath>

namespace aor {

// ── N3.dll API ──────────────────────────────────────────────────────────

namespace N3API {

using FnGetEngineInstance    = void*(__cdecl*)();
using FnGetActiveCamera      = void*(__thiscall*)(void* engine);
using FnIsFirstPerson        = bool(__thiscall*)(void* camera);
using FnGetClientControlDynel = void*(__thiscall*)(void* engine);
using FnGetGlobalPos = float*(__thiscall*)(void* dynel);
using FnSetRelRot = void(__thiscall*)(void* dynel, const void* quat);

static FnGetEngineInstance     GetEngineInstance     = nullptr;
static FnGetActiveCamera       GetActiveCamera       = nullptr;
static FnIsFirstPerson         IsFirstPerson         = nullptr;
static FnGetClientControlDynel GetClientControlDynel = nullptr;
static FnGetGlobalPos          GetGlobalPos          = nullptr;
static FnSetRelRot             SetRelRot             = nullptr;

static bool Init() {
    HMODULE n3 = GetModuleHandleA("N3.dll");
    if (!n3) {
        Log("[camera] N3.dll not loaded");
        return false;
    }
    Log("[camera] N3.dll at %p", n3);

    auto resolve = [&](const char* mangled, void** out, const char* name) -> bool {
        *out = reinterpret_cast<void*>(GetProcAddress(n3, mangled));
        if (!*out) {
            Log("[camera] FAILED to resolve %s", name);
            return false;
        }
        Log("[camera] resolved %s -> %p", name, *out);
        return true;
    };

    bool ok = true;
    ok &= resolve("?GetInstance@n3EngineClient_t@@SAPAV1@XZ",
        reinterpret_cast<void**>(&GetEngineInstance), "GetInstance");
    ok &= resolve("?GetActiveCamera@n3EngineClient_t@@QBEPAVn3Camera_t@@XZ",
        reinterpret_cast<void**>(&GetActiveCamera), "GetActiveCamera");
    ok &= resolve("?IsFirstPerson@n3Camera_t@@QBE_NXZ",
        reinterpret_cast<void**>(&IsFirstPerson), "IsFirstPerson");
    ok &= resolve("?GetClientControlDynel@n3EngineClient_t@@QBEPAVn3VisualDynel_t@@XZ",
        reinterpret_cast<void**>(&GetClientControlDynel), "GetClientControlDynel");
    ok &= resolve("?GetGlobalPos@n3Dynel_t@@QBEABVVector3_t@@XZ",
        reinterpret_cast<void**>(&GetGlobalPos), "GetGlobalPos");
    ok &= resolve("?SetRelRot@n3Dynel_t@@QAEXABVQuaternion_t@@@Z",
        reinterpret_cast<void**>(&SetRelRot), "SetRelRot");
    return ok;
}

}  // namespace N3API

// ── Constants ───────────────────────────────────────────────────────────

constexpr float kDefaultFollowSpeed = 5.0f;
constexpr float kMovementThreshold = 0.01f;

// ── Math helpers ────────────────────────────────────────────────────────

static float Vec3LenXZ(float x, float z) {
    return std::sqrt(x * x + z * z);
}

// ── CalcSteering hook ───────────────────────────────────────────────────

constexpr uint32_t kCalcSteeringRVA = 0x0001f752;

using FnCalcSteering = int(__thiscall*)(void* vehicle, void* result);
static FnCalcSteering g_origCalcSteering = nullptr;

static float g_lastPlayerX = 0.0f;
static float g_lastPlayerZ = 0.0f;
static bool  g_hasLastPos  = false;
static bool  g_rmbWasHeld  = false;

static int __fastcall CalcSteeringDetour(void* vehicle, void* /*edx*/, void* result) {
    ViewDistanceTick();   // not camera behaviour: just the per-frame call on the game thread it needs

    void* engine = N3API::GetEngineInstance ? N3API::GetEngineInstance() : nullptr;
    if (!engine) goto call_original;

    if (!IsCameraEnabled()) {
        g_rmbWasHeld = false;
        goto call_original;
    }

    // ── RMB-align & both-buttons continuous alignment ───────────────
    {
        const auto& input = GetInputState();
        bool rmbHeld = (input.state == MouseState::DRAGGING_RMB ||
                        input.state == MouseState::BOTH_HELD);
        bool rmbEdge = rmbHeld && !g_rmbWasHeld;
        g_rmbWasHeld = rmbHeld;

        bool bothActive = (input.state == MouseState::BOTH_HELD) &&
                          IsMouseRunEnabled();
        bool doAlign = (rmbEdge || bothActive) && N3API::SetRelRot;
        if (doAlign) {
            auto base = reinterpret_cast<uintptr_t>(vehicle);
            float hx = *reinterpret_cast<float*>(base + 0x1F8);
            float hz = *reinterpret_cast<float*>(base + 0x200);
            float lenXZ = Vec3LenXZ(hx, hz);

            if (lenXZ >= 0.001f) {
                constexpr float kPi = 3.14159265358979323846f;
                float beta  = std::atan2(hz, hx);
                float delta = -kPi * 0.5f - beta;
                while (delta >  kPi) delta -= 2.0f * kPi;
                while (delta < -kPi) delta += 2.0f * kPi;

                if (std::fabs(delta) > 0.001f) {
                    float qy = *reinterpret_cast<float*>(base + 0x170);
                    float qw = *reinterpret_cast<float*>(base + 0x178);
                    float currentAngle = 2.0f * std::atan2(qy, qw);
                    float newAngle = currentAngle + delta;

                    struct Q { float x, y, z, w; };
                    Q newQ{ 0.0f,
                            std::sin(newAngle * 0.5f),
                            0.0f,
                            std::cos(newAngle * 0.5f) };

                    *reinterpret_cast<float*>(base + 0x1F8) = 0.0f;
                    *reinterpret_cast<float*>(base + 0x200) = -lenXZ;

                    if (void* dynel = N3API::GetClientControlDynel(engine)) {
                        N3API::SetRelRot(dynel, &newQ);
                    }
                }
            }
        }
    }

    // ── Yaw follow during movement ──────────────────────────────────
    {
        const auto& input = GetInputState();
        void* dynel = N3API::GetClientControlDynel(engine);
        if (!dynel) goto call_original;

        float* pos = N3API::GetGlobalPos(dynel);
        if (!pos) goto call_original;

        float px = pos[0];
        float pz = pos[2];

        if (!g_hasLastPos) {
            g_lastPlayerX = px;
            g_lastPlayerZ = pz;
            g_hasLastPos = true;
            goto call_original;
        }

        float dx = px - g_lastPlayerX;
        float dz = pz - g_lastPlayerZ;
        float moveDist = Vec3LenXZ(dx, dz);

        g_lastPlayerX = px;
        g_lastPlayerZ = pz;

        if (moveDist < kMovementThreshold) goto call_original;

        // Don't follow while LMB-only is held (player is orbiting camera).
        if (input.state == MouseState::DRAGGING_LMB) goto call_original;

        auto base = reinterpret_cast<uintptr_t>(vehicle);

        float behindX = 0.0f;
        float behindZ = -1.0f;

        float* headingX = reinterpret_cast<float*>(base + 0x1F8);
        // headingY preserved at +0x1FC — pitch is never modified by yaw follow.
        float* headingZ = reinterpret_cast<float*>(base + 0x200);

        float dt = *reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(engine) + 0x68);

        float followSpeed = kDefaultFollowSpeed;
        AOVariant speedVal{};
        if (GameAPI::GetVariant("AOR_CYawSpd", speedVal)) {
            if (speedVal.type == static_cast<uint32_t>(VariantType::Double))
                followSpeed = static_cast<float>(speedVal.as_double);
            else if (speedVal.type == static_cast<uint32_t>(VariantType::Float))
                followSpeed = speedVal.as_float;
            else if (speedVal.type == static_cast<uint32_t>(VariantType::Int))
                followSpeed = static_cast<float>(speedVal.as_int);
        }

        float t = dt * followSpeed;
        if (t > 1.0f) t = 1.0f;

        float oldLenXZ = Vec3LenXZ(*headingX, *headingZ);
        if (oldLenXZ < 0.001f) goto call_original;

        float currentAngle = std::atan2(*headingZ, *headingX);
        float targetAngle  = std::atan2(behindZ, behindX);
        float diff = targetAngle - currentAngle;
        constexpr float kPi = 3.14159265358979323846f;
        if (diff >  kPi) diff -= 2.0f * kPi;
        if (diff < -kPi) diff += 2.0f * kPi;

        float newAngle = currentAngle + diff * t;
        *headingX = std::cos(newAngle) * oldLenXZ;
        *headingZ = std::sin(newAngle) * oldLenXZ;
    }

call_original:
    return g_origCalcSteering(vehicle, result);
}

// ── Public init ─────────────────────────────────────────────────────────

bool InitCameraHooks() {
    if (!N3API::Init()) {
        Log("[camera] N3 API init failed — camera hooks disabled");
        return false;
    }

    void* addr = ResolveRVA("N3.dll", kCalcSteeringRVA);
    if (!addr) {
        Log("[camera] CalcSteering RVA invalid");
        return false;
    }
    auto* bytes = static_cast<uint8_t*>(addr);
    Log("[camera] CalcSteering at %p, prologue: %02X %02X %02X %02X %02X",
        addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* tramp = nullptr;
    if (!InstallHook(addr, reinterpret_cast<void*>(&CalcSteeringDetour), &tramp)) {
        Log("[camera] failed to hook CalcSteering");
        return false;
    }
    g_origCalcSteering = reinterpret_cast<FnCalcSteering>(tramp);
    Log("[camera] CalcSteering hook installed");
    return true;
}

}  // namespace aor
