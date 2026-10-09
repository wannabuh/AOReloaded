#pragma once

// Characters and full-quality ground further out than the stock options allow.
//
// The options panel's "Character view distance" (DisplayCharViewDistance) and
// "Ground full quality" (DisplayGroundFullQualityRadius) sliders stop at 80 and
// 44 (LoginPrefs.xml). This raises both maximums, removes N3.dll's own 80 m
// character clamp, and remembers the values in AOReloaded.ini (AOR_CharDist,
// AOR_GroundHQ) so they come back after a restart.

#include "ao/types.h"

namespace aor {

// Patch N3's 80 m clamp and DisplaySystem's ground index offsets (which
// overflow past the stock radius). Call early, before the game world exists,
// so the game thread can't be running that code.
bool PatchViewDistanceCode();

// Raise the slider maximums. Call once the game world is up (the DValues
// exist) and after SettingsInstallHook().
bool InitViewDistance();

// Game thread, every frame (CalcSteering): puts the remembered values back
// once after login, when the client has clamped them while loading its prefs.
void ViewDistanceTick();

// From the SetDValue detour: remember a new slider value.
void ViewDistanceOnSetDValue(const char* name, const AOVariant& value);

}  // namespace aor
