#include "ui/widgets.h"

#include <imgui_internal.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace Hsdbg::Widgets
{
    namespace
    {
        ImFont* g_ui = nullptr;
        ImFont* g_strong = nullptr;
        [[maybe_unused]] ImFont* g_mono = nullptr;

        auto accent_color() -> ImVec4
        {
            return ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
        }

        // a colour that reads on top of the accent: dark ink on a light accent,
        // light ink on a dark one, so on-accent labels stay legible in any theme
        auto accent_ink() -> ImU32
        {
            const ImVec4 a = accent_color();
            const float luma = 0.299f * a.x + 0.587f * a.y + 0.114f * a.z;
            return luma > 0.6f ? IM_COL32(12, 16, 20, 255) : IM_COL32(245, 247, 250, 255);
        }

        auto with_alpha(ImVec4 colour, float alpha) -> ImU32
        {
            colour.w = alpha;
            return ImGui::ColorConvertFloat4ToU32(colour);
        }
    }

    auto set_fonts(ImFont* ui, ImFont* strong, ImFont* mono) -> void
    {
        g_ui = ui;
        g_strong = strong;
        g_mono = mono;
    }

    auto section_header(const char* label) -> void
    {
        std::string caps;
        caps.reserve(24);
        for (const char* c = label; *c != '\0'; ++c)
            caps.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(*c))));

        ImGui::Spacing();

        // small bold caps, a little dimmer than body text so it labels rather
        // than shouts; then a hairline out to the right edge of the content
        const float base = ImGui::GetStyle().FontSizeBase;
        ImGui::PushFont(g_strong, base * 0.80f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_Text, 0.58f));
        ImGui::TextUnformatted(caps.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();

        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        const float cy = (lo.y + hi.y) * 0.5f;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (right > hi.x + 10.0f)
            ImGui::GetWindowDrawList()->AddLine(ImVec2(hi.x + 8.0f, cy), ImVec2(right, cy),
                                                ImGui::GetColorU32(ImGuiCol_Border), 1.0f);
    }

    auto slider_float(const char* label, float* value, float min, float max, const char* fmt) -> bool
    {
        ImGui::PushID(label);

        const float width = ImGui::CalcItemWidth();
        const float height = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();

        ImGui::InvisibleButton("##track", ImVec2(width, height));
        const bool active = ImGui::IsItemActive();
        const bool hovered = ImGui::IsItemHovered();

        const float before = *value;
        if (active && ImGui::GetIO().MouseDown[0] && width > 0.0f)
        {
            const float t = ImClamp((ImGui::GetIO().MousePos.x - p.x) / width, 0.0f, 1.0f);
            *value = min + t * (max - min);
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float th = 6.0f;
        const float ty = p.y + (height - th) * 0.5f;
        const float frac = (max > min) ? ImClamp((*value - min) / (max - min), 0.0f, 1.0f) : 0.0f;
        const float gx = p.x + frac * width;
        const ImU32 accent = ImGui::ColorConvertFloat4ToU32(accent_color());

        draw->AddRectFilled(ImVec2(p.x, ty), ImVec2(p.x + width, ty + th),
                            ImGui::GetColorU32(ImGuiCol_FrameBg), th * 0.5f);
        draw->AddRectFilled(ImVec2(p.x, ty), ImVec2(gx, ty + th), accent, th * 0.5f);

        const float r = (active || hovered) ? height * 0.30f : height * 0.26f;
        draw->AddCircleFilled(ImVec2(gx, p.y + height * 0.5f), r, accent);
        draw->AddCircleFilled(ImVec2(gx, p.y + height * 0.5f), r * 0.42f, accent_ink());

        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, static_cast<double>(*value));
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        draw->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (height - ts.y) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_Text), buf);

        ImGui::PopID();

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);

        return *value != before;
    }

    auto icon_button(const char* id, const char* icon, bool active, const char* tooltip,
                     bool disabled, float size) -> bool
    {
        if (disabled)
            ImGui::BeginDisabled();

        ImGui::PushID(id);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, accent_color());

        // Button draws the bg + handles the click; the glyph is drawn by hand
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Button("##btn", ImVec2(size, size));

        if (active)
            ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // centre on the glyph's visual bounds so an asymmetric glyph (the play
        // triangle) sits dead-centre, not offset by its advance box or line-gap
        const ImU32 glyph_col = active ? accent_ink() : ImGui::GetColorU32(ImGuiCol_Text);
        const float cx = origin.x + size * 0.5f;
        const float cy = origin.y + size * 0.5f;

        unsigned int codepoint = 0;
        ImTextCharFromUtf8(&codepoint, icon, icon + std::strlen(icon));
        ImFontBaked* baked = ImGui::GetFontBaked();
        const ImFontGlyph* g = baked != nullptr ? baked->FindGlyph(static_cast<ImWchar>(codepoint))
                                                : nullptr;

        ImVec2 pen;
        if (g != nullptr)
            pen = ImVec2(cx - (g->X0 + g->X1) * 0.5f, cy - (g->Y0 + g->Y1) * 0.5f);
        else
            pen = ImVec2(cx - ImGui::CalcTextSize(icon).x * 0.5f, cy - ImGui::GetFontSize() * 0.5f);

        ImGui::GetWindowDrawList()->AddText(pen, glyph_col, icon);

        ImGui::PopID();

        if (disabled)
            ImGui::EndDisabled();

        if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tooltip);

        return clicked;
    }

    auto toggle(const char* id, bool* value) -> bool
    {
        const float height = ImGui::GetFrameHeight() * 0.78f;
        const float width = height * 1.8f;
        const ImVec2 pos = ImGui::GetCursorScreenPos();

        ImGui::InvisibleButton(id, ImVec2(width, height));
        const bool clicked = ImGui::IsItemClicked();
        if (clicked)
            *value = !*value;

        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();

        const float radius = height * 0.5f;
        const ImVec2 end(pos.x + width, pos.y + height);
        const ImU32 track = *value ? ImGui::ColorConvertFloat4ToU32(accent_color())
                                   : ImGui::GetColorU32(ImGuiCol_FrameBg);
        draw->AddRectFilled(pos, end, track, radius);
        if (!*value)
            draw->AddRect(pos, end, ImGui::GetColorU32(hovered ? ImGuiCol_Border : ImGuiCol_FrameBg),
                          radius);

        const float knob = radius - 2.5f;
        const float knob_x = *value ? (end.x - radius) : (pos.x + radius);
        const ImU32 knob_col = *value ? accent_ink() : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        draw->AddCircleFilled(ImVec2(knob_x, pos.y + radius), knob, knob_col);

        return clicked;
    }

    auto segmented(const char* id, const char* const* labels, int count, int current) -> int
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImDrawList* draw = ImGui::GetWindowDrawList();

        const float height = ImGui::GetFrameHeight();
        const float pad = 8.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // measure so the track wraps the labels exactly
        float total = 4.0f;
        for (int i = 0; i < count; ++i)
            total += ImGui::CalcTextSize(labels[i]).x + pad * 2.0f + 2.0f;

        const ImU32 track = ImGui::GetColorU32(ImGuiCol_FrameBg);
        draw->AddRectFilled(origin, ImVec2(origin.x + total, origin.y + height), track, height * 0.5f);

        int result = current;
        ImGui::PushID(id);
        float x = origin.x + 3.0f;
        const float seg_h = height - 6.0f;
        for (int i = 0; i < count; ++i)
        {
            const float w = ImGui::CalcTextSize(labels[i]).x + pad * 2.0f;
            const ImVec2 min(x, origin.y + 3.0f);
            const ImVec2 max(x + w, origin.y + 3.0f + seg_h);

            ImGui::SetCursorScreenPos(min);
            ImGui::InvisibleButton(labels[i], ImVec2(w, seg_h));
            const bool on = i == current;
            if (ImGui::IsItemClicked())
                result = i;
            const bool hovered = ImGui::IsItemHovered();

            if (on)
                draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_TabSelected), seg_h * 0.5f);
            else if (hovered)
                draw->AddRectFilled(min, max, with_alpha(style.Colors[ImGuiCol_Text], 0.06f),
                                    seg_h * 0.5f);

            const ImU32 text = ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
            draw->AddText(ImVec2(x + (w - ts.x) * 0.5f, origin.y + (height - ts.y) * 0.5f), text,
                          labels[i]);
            x += w + 2.0f;
        }
        ImGui::PopID();

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(total, height));
        return result;
    }

    auto selectable_row(const char* label, bool selected, ImGuiSelectableFlags flags) -> bool
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImDrawListSplitter split;
        split.Split(draw, 2);

        // content first, on the upper channel; the rounded fill goes behind it
        // once its rect and hover state are known
        split.SetCurrentChannel(draw, 1);
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
        const bool clicked = ImGui::Selectable(label, selected, flags);
        ImGui::PopStyleColor(3);

        const bool hovered = ImGui::IsItemHovered();
        if (selected || hovered)
        {
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 max = ImGui::GetItemRectMax();
            min.x += 2.0f;
            max.x -= 2.0f;
            const ImU32 fill = selected ? with_alpha(accent_color(), 0.18f)
                                        : with_alpha(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.055f);
            split.SetCurrentChannel(draw, 0);
            draw->AddRectFilled(min, max, fill, 7.0f);
            if (selected)
                draw->AddRect(min, max, with_alpha(accent_color(), 0.40f), 7.0f);
        }

        split.Merge(draw);
        return clicked;
    }

    auto chip(const char* text, ImU32 dot_color) -> void
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 ts = ImGui::CalcTextSize(text);

        const float dot = 7.0f;
        const float pad_x = 11.0f;
        const float gap = 8.0f;
        const float height = ImGui::GetFrameHeight();      // sit level with icon buttons
        const float width = pad_x + dot + gap + ts.x + pad_x;
        const ImVec2 end(pos.x + width, pos.y + height);

        draw->AddRectFilled(pos, end, ImGui::GetColorU32(ImGuiCol_FrameBg), height * 0.5f);
        draw->AddRect(pos, end, ImGui::GetColorU32(ImGuiCol_Border), height * 0.5f);
        draw->AddCircleFilled(ImVec2(pos.x + pad_x + dot * 0.5f, pos.y + height * 0.5f), dot * 0.5f,
                              dot_color);
        draw->AddText(ImVec2(pos.x + pad_x + dot + gap, pos.y + (height - ts.y) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_Text), text);

        ImGui::Dummy(ImVec2(width, height));
    }

    auto empty_state(const char* icon, const char* text) -> void
    {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float icon_size = ImGui::GetStyle().FontSizeBase * 2.3f;

        ImGui::PushFont(g_ui, icon_size);
        const ImVec2 is = ImGui::CalcTextSize(icon);
        ImGui::PopFont();
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const float total = is.y + ImGui::GetStyle().ItemSpacing.y + ts.y;

        const float ox = ImGui::GetCursorPosX();
        const float oy = ImGui::GetCursorPosY();
        if (avail.y > total)
            ImGui::SetCursorPosY(oy + (avail.y - total) * 0.5f);

        ImGui::SetCursorPosX(ox + ImMax(0.0f, (avail.x - is.x) * 0.5f));
        ImGui::PushFont(g_ui, icon_size);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.65f));
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::SetCursorPosX(ox + ImMax(0.0f, (avail.x - ts.x) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }

    auto remove_button(const char* id) -> bool
    {
        const float s = ImGui::GetTextLineHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();

        ImGui::InvisibleButton(id, ImVec2(s, s));
        const bool clicked = ImGui::IsItemClicked();
        const bool hovered = ImGui::IsItemHovered();

        const ImU32 col = hovered ? IM_COL32(232, 106, 100, 255)
                                  : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const float pad = s * 0.30f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddLine(ImVec2(p.x + pad, p.y + pad), ImVec2(p.x + s - pad, p.y + s - pad), col, 1.7f);
        draw->AddLine(ImVec2(p.x + s - pad, p.y + pad), ImVec2(p.x + pad, p.y + s - pad), col, 1.7f);

        return clicked;
    }
}
