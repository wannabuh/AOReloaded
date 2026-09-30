#pragma once

// Frame rate cap — replaces the client's built-in 100 FPS limit.
//
// AFCM.dll's frame limiter (Timer_t::FrameProcess) Sleep()s until 1/cap
// seconds have passed since the previous frame, where cap is the static
// float Timer_t::m_vMaximumFramerate (stock value 100). AFCM.dll exports
// that variable, and nothing in the client ever writes it, so setting it
// once (and again whenever the slider moves) is all that's needed.
//
// Controlled by the AOR_FpsCap DValue (default: 100, the stock value).

namespace aor {

// Resolve Timer_t::m_vMaximumFramerate and apply the AOR_FpsCap setting.
// Call after SettingsRegisterAll(). AFCM.dll is a static import of
// AnarchyOnline.exe, so it is always loaded by then.
// Returns true if the cap could be located.
bool InitFpsCap();

}  // namespace aor
