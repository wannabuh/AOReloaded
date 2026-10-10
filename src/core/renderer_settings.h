#pragma once

// Settings of a custom renderer (randy-vk's randy31.dll) in the options panel.
//
// If the loaded randy31.dll exports the randy-vk settings interface (RvkSettings_*), its settings get a
// "Renderer" tab: each becomes a DValue (bool, or int - float settings are scaled by their step) bound to an
// option widget, and changes made there are passed to the renderer, which applies and saves them itself
// (randy-vk.ini). Without such a renderer nothing happens and no tab is added.

#include "ao/types.h"

namespace aor {

// Resolve the interface and register the DValues. Call with the other settings (before RegisterRootXmlOverlay).
void RendererSettingsRegisterAll();

// The Renderer tab's XML (a ScrollView), or nullptr without a renderer interface.
const char* RendererXmlBlock();

// For the SetDValue detour: a renderer setting changed in the options panel. True if `name` is one.
bool RendererOnSetDValue(const char* name, const AOVariant& value);

}  // namespace aor
