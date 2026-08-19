#pragma once

// Shared visual language for BardHero's modal FLICK panels. Callers include
// SimpleIni + FUCK_API before this header (house include ordering).

#include "render/PanelStyleLogic.h"
#include "render/Theme.h"

namespace SH::panel {
    inline ImVec4 kText{ 0.88f, 0.88f, 0.85f, 1.0f };
    inline ImVec4 kQuiet{ 0.62f, 0.62f, 0.59f, 0.95f };
    inline ImVec4 kGold{ 0.88f, 0.70f, 0.32f, 1.0f };
    inline ImVec4 kBlue{ 0.48f, 0.78f, 0.90f, 1.0f };
    inline ImVec4 kAccent{ 0.46f, 0.46f, 0.43f, 0.82f };

    inline void ApplyTheme(const theme::ThemeData& t) {
        kText = t.text;
        kQuiet = t.muted;
        kGold = t.highlight;
        kBlue = t.accent;
        kAccent = t.accent;
    }

    inline void Draw(const ImVec2& lo, const ImVec2& hi,
                     const ImVec4& accent = kAccent,
                     float alpha = 0.86f,
                     const panel_style::SurfaceStyle& surface =
                         panel_style::ModalSurface()) {
        const auto& t = theme::Get();
        const float s = FUCK::Scale(1.0f);
        const float rounding = std::max(
            0.0f, (t.rounding + surface.rounding - 9.0f) * s);
        const float shadowOffset = std::max(
            0.0f, t.shadowOffset + surface.shadowOffset - 5.0f);
        if (shadowOffset > 0.0f) {
            const float shadow = shadowOffset * s;
            FUCK::DrawRectFilled(ImVec2(lo.x + shadow, lo.y + shadow),
                                 ImVec2(hi.x + shadow, hi.y + shadow),
                                 ImVec4(0, 0, 0, 0.34f), rounding);
        }
        ImVec4 fill = t.panel;
        fill.w = std::clamp(fill.w * (alpha / 0.86f), 0.0f, 1.0f);
        FUCK::DrawRectFilled(lo, hi, fill, rounding);
        FUCK::DrawRect(lo, hi, t.border, rounding, 3 * s);
        if (t.innerBorder) {
            const float innerRounding = std::max(0.0f, rounding - 3 * s);
            FUCK::DrawRect(ImVec2(lo.x + 6 * s, lo.y + 6 * s),
                           ImVec2(hi.x - 6 * s, hi.y - 6 * s), accent,
                           innerRounding, s);
        }
    }

    inline void DrawCurrent(const ImVec4& accent = kAccent,
                            float alpha = 0.86f,
                            const panel_style::SurfaceStyle& surface =
                                panel_style::ModalSurface()) {
        const float s = FUCK::Scale(1.0f);
        const ImVec2 p = FUCK::GetCursorScreenPos();
        const ImVec2 a = FUCK::GetContentRegionAvail();
        Draw(ImVec2(p.x + surface.nearInset * s,
                    p.y + surface.nearInset * s),
             ImVec2(p.x + a.x - surface.farInset * s,
                    p.y + a.y - surface.farInset * s),
             accent, alpha, surface);
    }

    inline void BeginContent(float logicalTop = 14.0f) {
        const ImVec2 p = FUCK::GetCursorPos();
        FUCK::SetCursorPosY(p.y + FUCK::Scale(logicalTop));
    }

    inline void BeginBoundedContent(const char* id,
                                    float logicalInset = 20.0f,
                                    int childFlags = 0) {
        const float inset = FUCK::Scale(logicalInset);
        const ImVec2 p = FUCK::GetCursorPos();
        const ImVec2 a = FUCK::GetContentRegionAvail();
        FUCK::SetCursorPos(ImVec2(p.x + inset, p.y + inset));
        FUCK::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        FUCK::BeginChild(id,
                         ImVec2(std::max(1.0f, a.x - inset * 2.0f),
                                std::max(1.0f, a.y - inset * 2.0f)),
                         false, childFlags);
        FUCK::PopStyleVar();
    }

    inline void EndBoundedContent() { FUCK::EndChild(); }

    inline void Header(const char* title) {
        FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
        FUCK::CenteredText(title, false);
        FUCK::PopFont();
        const float s = FUCK::Scale(1.0f);
        const ImVec2 p = FUCK::GetCursorScreenPos();
        const float w = FUCK::GetContentRegionAvail().x;
        ImVec4 line = theme::Get().accent;
        line.w *= 0.85f;
        FUCK::DrawLine(ImVec2(p.x + 12 * s, p.y + 2 * s),
                       ImVec2(p.x + w - 12 * s, p.y + 2 * s),
                       line, s);
        FUCK::Dummy(ImVec2(1.0f, 10.0f * s));
    }

    inline void PushControls() {
        const auto& t = theme::Get();
        const ImVec4 white(1,1,1,1);
        const ImVec4 black(0,0,0,1);
        FUCK::PushStyleColor(ImGuiCol_Button, t.control);
        FUCK::PushStyleColor(ImGuiCol_ButtonHovered,
                             theme::Mix(t.control, t.highlight, 0.28f));
        FUCK::PushStyleColor(ImGuiCol_ButtonActive,
                             theme::Mix(t.control, t.highlight, 0.48f));
        FUCK::PushStyleColor(ImGuiCol_Header,
                             theme::Mix(t.control, t.accent, 0.34f));
        FUCK::PushStyleColor(ImGuiCol_HeaderHovered,
                             theme::Mix(t.control, t.accent, 0.58f));
        FUCK::PushStyleColor(ImGuiCol_HeaderActive,
                             theme::Mix(t.control, t.accent, 0.78f));
        FUCK::PushStyleColor(ImGuiCol_FrameBg,
                             theme::Mix(t.control, black, 0.25f));
        FUCK::PushStyleColor(ImGuiCol_FrameBgHovered,
                             theme::Mix(t.control, t.accent, 0.30f));
        FUCK::PushStyleColor(ImGuiCol_FrameBgActive,
                             theme::Mix(t.control, t.accent, 0.48f));
        FUCK::PushStyleColor(ImGuiCol_Border, t.border);
        FUCK::PushStyleColor(ImGuiCol_TableHeaderBg,
                             theme::Mix(t.panel, t.control, 0.62f));
        FUCK::PushStyleColor(ImGuiCol_TableRowBg,
                             theme::Mix(t.panel, black, 0.08f));
        FUCK::PushStyleColor(ImGuiCol_TableRowBgAlt,
                             theme::Mix(t.panel, white, 0.04f));
    }

    inline void PopControls() { FUCK::PopStyleColor(13); }

    inline bool Contains(const ImVec2& p, const ImVec2& lo,
                         const ImVec2& hi) {
        return p.x >= lo.x && p.y >= lo.y && p.x < hi.x && p.y < hi.y;
    }

    inline void ButtonFrame(const ImVec2& lo, const ImVec2& size,
                            bool hot, bool focused = false) {
        const auto& t = theme::Get();
        const float s = FUCK::Scale(1.0f);
        const ImVec2 hi(lo.x + size.x, lo.y + size.y);
        const ImVec4 fill = hot
            ? theme::Mix(t.control, t.highlight, 0.34f)
            : focused ? theme::Mix(t.control, t.accent, 0.30f)
                      : t.control;
        FUCK::DrawRectFilled(lo, hi, fill, 4 * s);
        const ImVec4 edge = hot || focused
            ? theme::Mix(t.border, t.highlight, 0.55f)
            : t.border;
        FUCK::DrawRect(lo, hi, edge, 4 * s,
                       (hot || focused) ? 2 * s : s);
    }
}
