// fps_cap.cpp — Frame rate cap.
//
// Writes the AOR_FpsCap setting into AFCM.dll's Timer_t::m_vMaximumFramerate.
// The only code that touches that variable is the limiter in
// Timer_t::FrameProcess (AFCM.dll 0x100065d9: flds m_vMaximumFramerate), which
// reads it every frame, so changes apply on the next frame.
//
// See fps_cap.h for the design overview.

#include "core/fps_cap.h"
#include "core/settings.h"
#include "core/logging.h"
#include "ao/game_api.h"
#include "ao/types.h"

#include <windows.h>
#include <cstring>

namespace aor {

// Stock client value, also the AOR_FpsCap default.
static constexpr int kStockFps = 100;

// The limiter divides by the cap, so never let it reach zero. Matches the
// AOR_FpsCap slider range in settings.cpp.
static constexpr int kMinFps = 30;
static constexpr int kMaxFps = 500;

static float* g_maxFramerate = nullptr;

static void ApplyFpsCap(int fps) {
    if (!g_maxFramerate) return;

    if (fps < kMinFps) fps = kMinFps;
    if (fps > kMaxFps) fps = kMaxFps;

    // Aligned 4-byte store: atomic on x86, so the game thread reading it
    // every frame never sees a torn value.
    *g_maxFramerate = static_cast<float>(fps);
    Log("[fpscap] maximum frame rate set to %d", fps);
}

static void OnSettingChanged(const char* name, int newValue) {
    if (std::strcmp(name, "AOR_FpsCap") == 0)
        ApplyFpsCap(newValue);
}

const char kFpsCapSliderXml[] =
    "        <OptionSlider label=\"Maximum frame rate (default: 100):\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_FpsCap\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n";

bool InitFpsCap() {
    HMODULE afcm = GetModuleHandleA("AFCM.dll");
    if (!afcm) {
        Log("[fpscap] AFCM.dll not loaded");
        return false;
    }

    // static float Timer_t::m_vMaximumFramerate
    g_maxFramerate = reinterpret_cast<float*>(
        GetProcAddress(afcm, "?m_vMaximumFramerate@Timer_t@@2MA"));
    if (!g_maxFramerate) {
        Log("[fpscap] Timer_t::m_vMaximumFramerate export not found");
        return false;
    }

    Log("[fpscap] client cap was %.0f", *g_maxFramerate);

    int fps = kStockFps;
    AOVariant v;
    if (GameAPI::GetVariant("AOR_FpsCap", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        fps = v.as_int;
    }

    ApplyFpsCap(fps);
    RegisterSettingCallback(&OnSettingChanged);
    return true;
}

}  // namespace aor
