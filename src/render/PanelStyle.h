#pragma once

// Shared visual language for BardHero's modal FLICK panels. Callers include
// SimpleIni + FUCK_API before this header (house include ordering).

#include "render/PanelStyleLogic.h"

namespace SH::panel {
    inline constexpr ImVec4 kText{ 0.88f, 0.88f, 0.85f, 1.0f };
    inline constexpr ImVec4 kQuiet{ 0.62f, 0.62f, 0.59f, 0.95f };
    inline constexpr ImVec4 kGold{ 0.88f, 0.70f, 0.32f, 1.0f };
    inline constexpr ImVec4 kBlue{ 0.48f, 0.78f, 0.90f, 1.0f };
    inline constexpr ImVec4 kAccent{ 0.46f, 0.46f, 0.43f, 0.82f };

    inline void Draw(const ImVec2& lo, const ImVec2& hi,
                     const ImVec4& accent = kAccent,
                     float alpha = 0.86f,
                     const panel_style::SurfaceStyle& surface =
                         panel_style::ModalSurface()) {
        const float s = FUCK::Scale(1.0f);
        const float rounding = surface.rounding * s;
        if (surface.shadowOffset > 0.0f) {
            const float shadow = surface.shadowOffset * s;
            FUCK::DrawRectFilled(ImVec2(lo.x + shadow, lo.y + shadow),
                                 ImVec2(hi.x + shadow, hi.y + shadow),
                                 ImVec4(0, 0, 0, 0.34f), rounding);
        }
        FUCK::DrawRectFilled(lo, hi,
                             ImVec4(0.032f, 0.032f, 0.030f, alpha),
                             rounding);
        FUCK::DrawRect(lo, hi, ImVec4(0.48f, 0.48f, 0.45f, 0.94f),
                       rounding, 3 * s);
        const float innerRounding =
            std::max(0.0f, surface.rounding - 3.0f) * s;
        FUCK::DrawRect(ImVec2(lo.x + 6 * s, lo.y + 6 * s),
                       ImVec2(hi.x - 6 * s, hi.y - 6 * s), accent,
                       innerRounding, s);
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

    // FLICK calls IWindow::Draw inside its own content child. Drawing an
    // inset frame does not alter that child's layout bounds, so ordinary
    // widgets still consume the full host width unless we give them a real
    // clipped child. This is the containment contract for modal panels.
    inline void BeginBoundedContent(const char* id,
                                    float logicalInset = 20.0f,
                                    int childFlags = 0) {
        const float  inset = FUCK::Scale(logicalInset);
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
        FUCK::DrawLine(ImVec2(p.x + 12 * s, p.y + 2 * s),
                       ImVec2(p.x + w - 12 * s, p.y + 2 * s),
                       ImVec4(0.46f, 0.46f, 0.43f, 0.70f), s);
        FUCK::Dummy(ImVec2(1.0f, 10.0f * s));
    }

    inline void PushControls() {
        FUCK::PushStyleColor(ImGuiCol_Button,
                             ImVec4(0.16f, 0.16f, 0.15f, 0.94f));
        FUCK::PushStyleColor(ImGuiCol_ButtonHovered,
                             ImVec4(0.28f, 0.28f, 0.26f, 0.98f));
        FUCK::PushStyleColor(ImGuiCol_ButtonActive,
                             ImVec4(0.38f, 0.38f, 0.35f, 1.0f));
        FUCK::PushStyleColor(ImGuiCol_Header,
                             ImVec4(0.22f, 0.22f, 0.21f, 0.86f));
        FUCK::PushStyleColor(ImGuiCol_HeaderHovered,
                             ImVec4(0.32f, 0.32f, 0.30f, 0.94f));
        FUCK::PushStyleColor(ImGuiCol_HeaderActive,
                             ImVec4(0.40f, 0.40f, 0.37f, 1.0f));
        FUCK::PushStyleColor(ImGuiCol_FrameBg,
                             ImVec4(0.11f, 0.11f, 0.105f, 0.94f));
        FUCK::PushStyleColor(ImGuiCol_FrameBgHovered,
                             ImVec4(0.21f, 0.21f, 0.20f, 0.96f));
        FUCK::PushStyleColor(ImGuiCol_FrameBgActive,
                             ImVec4(0.28f, 0.28f, 0.26f, 0.98f));
        FUCK::PushStyleColor(ImGuiCol_Border,
                             ImVec4(0.52f, 0.52f, 0.48f, 0.86f));
        FUCK::PushStyleColor(ImGuiCol_TableHeaderBg,
                             ImVec4(0.13f, 0.13f, 0.12f, 0.98f));
        FUCK::PushStyleColor(ImGuiCol_TableRowBg,
                             ImVec4(0.055f, 0.055f, 0.052f, 0.82f));
        FUCK::PushStyleColor(ImGuiCol_TableRowBgAlt,
                             ImVec4(0.080f, 0.080f, 0.075f, 0.82f));
    }

    inline void PopControls() { FUCK::PopStyleColor(13); }

    inline bool Contains(const ImVec2& p, const ImVec2& lo,
                         const ImVec2& hi) {
        return p.x >= lo.x && p.y >= lo.y && p.x < hi.x && p.y < hi.y;
    }

    inline void ButtonFrame(const ImVec2& lo, const ImVec2& size,
                            bool hot, bool focused = false) {
        const float s = FUCK::Scale(1.0f);
        const ImVec2 hi(lo.x + size.x, lo.y + size.y);
        const ImVec4 fill = hot
            ? ImVec4(0.34f, 0.34f, 0.31f, 0.98f)
            : focused ? ImVec4(0.25f, 0.25f, 0.23f, 0.96f)
                      : ImVec4(0.17f, 0.17f, 0.16f, 0.94f);
        FUCK::DrawRectFilled(lo, hi, fill, 4 * s);
        FUCK::DrawRect(lo, hi,
                       hot || focused
                           ? ImVec4(0.74f, 0.74f, 0.68f, 1.0f)
                           : ImVec4(0.55f, 0.55f, 0.51f, 0.95f),
                       4 * s, (hot || focused) ? 2 * s : s);
    }
}
