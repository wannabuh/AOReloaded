// Renderer settings tab (see renderer_settings.h).

#include "core/renderer_settings.h"

#include "ao/game_api.h"
#include "core/logging.h"
#include "core/settings.h"

#include <windows.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace aor {

namespace {

// randy-vk's exported interface (proxy/ddraw/rvk_settings.h in randy-vk), versions 1 and 2. Version 2 adds the
// choice type and the fields from `parent` on (a version 1 renderer leaves them null).
struct RvkSettingInfo {
    uint32_t size;
    const char* name;
    const char* label;
    const char* section;
    uint32_t type;              // 0 bool, 1 int, 2 float, 3 choice (an int from `choices`)
    float min, max, step;
    float value, defaultValue;
    const char* parent;         // the bool switching the feature this setting belongs to (null: none)
    const char* choices;        // choice: the allowed values separated by spaces
};
enum : uint32_t { kBool = 0, kInt = 1, kFloat = 2, kChoice = 3 };
using FnVersion = uint32_t (*)();
using FnCount = uint32_t (*)();
using FnGet = int (*)(uint32_t, RvkSettingInfo*);
using FnSet = int (*)(const char*, float);

FnCount g_count;
FnGet g_get;
FnSet g_set;
uint32_t g_version;
bool g_available;
char g_xml[64 * 1024];

bool Resolve()
{
    HMODULE randy = GetModuleHandleA("randy31.dll");
    if (!randy) return false;
    auto version = reinterpret_cast<FnVersion>(GetProcAddress(randy, "RvkSettings_Version"));
    g_count = reinterpret_cast<FnCount>(GetProcAddress(randy, "RvkSettings_Count"));
    g_get = reinterpret_cast<FnGet>(GetProcAddress(randy, "RvkSettings_Get"));
    g_set = reinterpret_cast<FnSet>(GetProcAddress(randy, "RvkSettings_Set"));
    if (!version || !g_count || !g_get || !g_set) return false;
    g_version = version();
    if (g_version != 1 && g_version != 2) {
        Log("[renderer] unknown settings interface version %u", g_version);
        return false;
    }
    return true;
}

bool GetInfo(uint32_t i, RvkSettingInfo& info)
{
    std::memset(&info, 0, sizeof(info));
    // A version 1 renderer only accepts its own (smaller) size.
    info.size = g_version >= 2 ? sizeof(info) : static_cast<uint32_t>(offsetof(RvkSettingInfo, parent));
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

bool Is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

void AppendCheckBox(char*& out, char* end, const RvkSettingInfo& s, int indent)
{
    char attrs[128];
    std::snprintf(attrs, sizeof(attrs), "\" layout_borders=\"Rect(%d,0,0,0)\" opt_type=\"variant\" opt_variable=\"", indent);
    Append(out, end, "        <OptionCheckBox label=\"");
    AppendEscaped(out, end, s.label);
    Append(out, end, attrs);
    Append(out, end, s.name);
    Append(out, end, "\"/>\n");
}

// A slider (int: the value; float: steps, shown scaled back) or, for a choice, radio buttons of its values.
void AppendValue(char*& out, char* end, const RvkSettingInfo& s)
{
    if (s.type == kChoice && s.choices) {
        Append(out, end, "        <OptionRadioButtonGroup label=\"");
        AppendEscaped(out, end, s.label);
        Append(out, end, ":\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"");
        Append(out, end, s.name);
        Append(out, end, "\">\n");
        for (const char* c = s.choices; *c;) {
            while (*c == ' ') ++c;
            const char* e = c;
            while (*e && *e != ' ') ++e;
            if (e == c) break;
            char value[32];
            std::snprintf(value, sizeof(value), "%.*s", static_cast<int>(e - c), c);
            Append(out, end, "          <RadioButton label=\"");
            Append(out, end, value);
            Append(out, end, "\" value=\"");
            Append(out, end, value);
            Append(out, end, "\"/>\n");
            c = e;
        }
        Append(out, end, "        </OptionRadioButtonGroup>\n");
        return;
    }
    char attrs[160];
    if (s.type == kFloat)
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

void AppendHeading(char*& out, char* end, const char* text, int top)
{
    char attrs[64];
    std::snprintf(attrs, sizeof(attrs), "\" layout_borders=\"Rect(0,%d,0,3)\" />\n", top);
    Append(out, end, "\n        <TextView value=\"");
    AppendEscaped(out, end, text);
    Append(out, end, attrs);
}

// Version 2 layout: every on / off option at the top (by section, a feature's own options indented under it), then
// the values (sliders, choices) in blocks headed by their feature.
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
    uint32_t count = g_count();
    std::vector<RvkSettingInfo> all;
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (GetInfo(i, s)) all.push_back(s);
    }
    auto childrenOf = [&](const RvkSettingInfo& p, bool bools) {
        std::vector<const RvkSettingInfo*> list;
        for (const RvkSettingInfo& c : all)
            if (Is(c.parent, p.name) && (c.type == kBool) == bools) list.push_back(&c);
        return list;
    };

    // Options: the switches.
    AppendHeading(out, end, "Options", 0);
    const char* section = "";
    for (const RvkSettingInfo& s : all) {
        if (s.type != kBool || s.parent) continue;
        if (!Is(section, s.section)) {
            section = s.section;
            AppendHeading(out, end, section, 6);
        }
        AppendCheckBox(out, end, s, 10);
        for (const RvkSettingInfo* c : childrenOf(s, true)) AppendCheckBox(out, end, *c, 30);
    }

    // Values: a block per feature (its label) or, for values of no feature, per section.
    AppendHeading(out, end, "Adjustments", 16);
    for (const RvkSettingInfo& s : all) {
        if (s.type == kBool && !s.parent) {
            std::vector<const RvkSettingInfo*> values = childrenOf(s, false);
            if (values.empty()) continue;
            AppendHeading(out, end, s.label, 8);
            for (const RvkSettingInfo* v : values) AppendValue(out, end, *v);
        } else if (s.type != kBool && !s.parent) {
            AppendHeading(out, end, s.section, 8);
            AppendValue(out, end, s);
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
    if (out >= g_xml + sizeof(g_xml) - 300)
        Log("[renderer] options XML truncated (%u bytes)", static_cast<unsigned>(out - g_xml));
}

// Version 1 renderers: in table order, sections as headings.
void BuildXmlV1()
{
    char* out = g_xml;
    char* end = g_xml + sizeof(g_xml) - 256;
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
        if (!Is(section, s.section)) {
            section = s.section;
            AppendHeading(out, end, section, 10);
        }
        if (s.type == kBool) AppendCheckBox(out, end, s, 10);
        else AppendValue(out, end, s);
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
        if (s.type == kBool) {
            GameAPI::RegisterBool(s.name, s.value != 0.0f);
        } else if (s.type == kInt || s.type == kChoice) {
            GameAPI::RegisterInt(s.name, static_cast<int>(s.value));
            SetDValueMinMax(s.name, static_cast<int>(s.min), static_cast<int>(s.max));
        } else {
            GameAPI::RegisterInt(s.name, ToSteps(s, s.value));
            SetDValueMinMax(s.name, ToSteps(s, s.min), ToSteps(s, s.max));
        }
    }
    if (g_version >= 2) BuildXml();
    else BuildXmlV1();
    Log("[renderer] %u renderer settings registered (interface %u, options XML %u bytes)", count, g_version,
        static_cast<unsigned>(std::strlen(g_xml)));
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
        float setting = s.type == kFloat ? static_cast<float>(std::round(v) * s.step) : static_cast<float>(v);
        if (s.type == kBool) setting = setting != 0.0f ? 1.0f : 0.0f;
        g_set(name, setting);
        Log("[renderer] %s = %g", name, setting);
        return true;
    }
    return false;
}

}  // namespace aor
