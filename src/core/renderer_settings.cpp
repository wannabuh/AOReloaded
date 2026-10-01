// Renderer settings tab (see renderer_settings.h).

#include "core/renderer_settings.h"

#include "ao/game_api.h"
#include "core/logging.h"
#include "core/settings.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace aor {

namespace {

// randy-vk's exported interface (proxy/ddraw/rvk_settings.h in randy-vk), version 1.
struct RvkSettingInfo {
    uint32_t size;
    const char* name;
    const char* label;
    const char* section;
    uint32_t type;              // 0 bool, 1 int, 2 float
    float min, max, step;
    float value, defaultValue;
};
using FnVersion = uint32_t (*)();
using FnCount = uint32_t (*)();
using FnGet = int (*)(uint32_t, RvkSettingInfo*);
using FnSet = int (*)(const char*, float);

FnCount g_count;
FnGet g_get;
FnSet g_set;
bool g_available;
char g_xml[24 * 1024];

bool Resolve()
{
    HMODULE randy = GetModuleHandleA("randy31.dll");
    if (!randy) return false;
    auto version = reinterpret_cast<FnVersion>(GetProcAddress(randy, "RvkSettings_Version"));
    g_count = reinterpret_cast<FnCount>(GetProcAddress(randy, "RvkSettings_Count"));
    g_get = reinterpret_cast<FnGet>(GetProcAddress(randy, "RvkSettings_Get"));
    g_set = reinterpret_cast<FnSet>(GetProcAddress(randy, "RvkSettings_Set"));
    if (!version || !g_count || !g_get || !g_set) return false;
    if (version() != 1) {
        Log("[renderer] unknown settings interface version %u", version());
        return false;
    }
    return true;
}

bool GetInfo(uint32_t i, RvkSettingInfo& info)
{
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    return g_get(i, &info) != 0;
}

// Float settings live in int DValues counting steps.
int ToSteps(const RvkSettingInfo& s, float v) { return static_cast<int>(std::lround(v / s.step)); }

// XML attribute text: escape the characters XML needs escaped.
void AppendEscaped(char*& out, char* end, const char* text)
{
    for (; *text && out < end - 8; ++text) {
        const char* rep = nullptr;
        switch (*text) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        }
        if (rep) { size_t n = std::strlen(rep); std::memcpy(out, rep, n); out += n; }
        else *out++ = *text;
    }
}

void Append(char*& out, char* end, const char* text)
{
    size_t n = std::strlen(text);
    if (out + n >= end) n = static_cast<size_t>(end - out - 1);
    std::memcpy(out, text, n);
    out += n;
}

void BuildXml()
{
    char* out = g_xml;
    char* end = g_xml + sizeof(g_xml) - 256;   // room for the closing tags
    Append(out, end,
           "\n"
           "  <ScrollView h_alignment=\"LEFT\" label=\"Renderer\" v_scrollbar_mode=\"auto\""
           " scroll_client=\"rvk_scroll\" max_size=\"Point(16000,-1)\">\n"
           "    <ScrollViewChild name=\"rvk_scroll\">\n"
           "      <View h_alignment=\"LEFT\" view_layout=\"vertical\""
           " max_size=\"Point(16000,-1)\" layout_borders=\"Rect(0,0,10,0)\">\n"
           "        <TextView value=\"randy-vk renderer\" layout_borders=\"Rect(0,0,0,5)\" />\n"
           "        <TextView value=\"Changes apply at once. Settings are saved to randy-vk.ini.\""
           " layout_borders=\"Rect(0,0,0,10)\" />\n");
    const char* section = "";
    uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s)) continue;
        if (std::strcmp(section, s.section) != 0) {
            section = s.section;
            Append(out, end, "\n        <TextView value=\"");
            AppendEscaped(out, end, section);
            Append(out, end, "\" layout_borders=\"Rect(0,10,0,3)\" />\n");
        }
        if (s.type == 0) {
            Append(out, end, "        <OptionCheckBox label=\"");
            AppendEscaped(out, end, s.label);
            Append(out, end, "\" layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"");
            Append(out, end, s.name);
            Append(out, end, "\"/>\n");
        } else {
            // Int sliders show the value; float ones count steps, shown scaled back.
            char attrs[160];
            if (s.type == 2)
                std::snprintf(attrs, sizeof(attrs),
                              "\" value_fmt=\"&lt;font color=#70C4D0&gt;%%.2f&lt;/font&gt;\" value_scale=\"%g\"/>\n", s.step);
            else
                std::snprintf(attrs, sizeof(attrs),
                              "\" value_fmt=\"&lt;font color=#70C4D0&gt;%%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n");
            Append(out, end, "        <OptionSlider label=\"");
            AppendEscaped(out, end, s.label);
            Append(out, end, ":\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"");
            Append(out, end, s.name);
            Append(out, end, attrs);
        }
    }
    end = g_xml + sizeof(g_xml) - 1;
    Append(out, end,
           "\n"
           "        <VLayoutSpacer/>\n"
           "      </View>\n"
           "    </ScrollViewChild>\n"
           "  </ScrollView>\n");
    *out = '\0';
}

}  // namespace

void RendererSettingsRegisterAll()
{
    g_available = Resolve();
    if (!g_available) {
        Log("[renderer] no randy-vk settings interface in randy31.dll - no Renderer tab");
        return;
    }
    uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s) || std::strlen(s.name) > 15) continue;
        if (s.type == 0) {
            GameAPI::RegisterBool(s.name, s.value != 0.0f);
        } else if (s.type == 1) {
            GameAPI::RegisterInt(s.name, static_cast<int>(s.value));
            SetDValueMinMax(s.name, static_cast<int>(s.min), static_cast<int>(s.max));
        } else {
            GameAPI::RegisterInt(s.name, ToSteps(s, s.value));
            SetDValueMinMax(s.name, ToSteps(s, s.min), ToSteps(s, s.max));
        }
    }
    BuildXml();
    Log("[renderer] %u renderer settings registered", count);
}

const char* RendererXmlBlock() { return g_available ? g_xml : nullptr; }

bool RendererOnSetDValue(const char* name, const AOVariant& value)
{
    if (!g_available || std::strncmp(name, "RVK_", 4) != 0)
        return false;
    uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s) || std::strcmp(s.name, name) != 0) continue;
        double v = 0.0;
        switch (value.type) {
        case static_cast<uint32_t>(VariantType::Bool): v = value.as_bool ? 1.0 : 0.0; break;
        case static_cast<uint32_t>(VariantType::Int): v = value.as_int; break;
        case static_cast<uint32_t>(VariantType::Float): v = value.as_float; break;
        case static_cast<uint32_t>(VariantType::Double): v = value.as_double; break;
        default: return true;
        }
        float setting = s.type == 2 ? static_cast<float>(std::round(v) * s.step) : static_cast<float>(v);
        if (s.type == 0) setting = setting != 0.0f ? 1.0f : 0.0f;
        g_set(name, setting);
        Log("[renderer] %s = %g", name, setting);
        return true;
    }
    return false;
}

}  // namespace aor
