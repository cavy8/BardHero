// src/render/RenderUi.cpp
#include "PCH.h"
#include "render/RenderUi.h"

#include "Settings.h"
#include "game/EngineFeed.h"
#include "render/BrowserWindow.h"
#include "render/HighwayLayout.h"
#include "render/HighwayWindow.h"
#include "render/HudWindow.h"
#include "render/PauseMenuWindow.h"
#include "render/ResultsWindow.h"
#include "render/SettingsTool.h"
#include "render/Theme.h"
#include "render/FlickWindowPolicy.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"
#include "render/PanelStyle.h"

#include <cmath>

namespace SH::RenderUi {
    namespace {
        int g_cursorRefs = 0;  // render thread only

        hw::RGBA ToRgba(const ImVec4& c) {
            return { c.x, c.y, c.z, c.w };
        }

        void ApplyTheme() {
            const auto& t = theme::Get();
            panel::ApplyTheme(t);
            for (int i = 0; i < 5; ++i) hw::kLaneColors[i] = ToRgba(t.fret[i]);
            hw::kOpenColor    = ToRgba(t.openNote);
            hw::kMissGrey     = ToRgba(t.miss);
            hw::kSpActiveCyan = ToRgba(t.starPower);
        }

        class ThemeHighwayBackground final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "ThemeHighwayBackground"; }
            const char* Title() const override { return "BardHero Highway Background"; }
            bool IsOpen() const override {
                return !theme::Get().highwayBackground.empty() &&
                       EngineFeed::GetSingleton().active.load(std::memory_order_acquire);
            }
            void SetOpen(bool) override {}
            void Draw() override {}
            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                return static_cast<F>(flick_window_policy::HighwayHostFlags(
                    static_cast<unsigned>(F::kNoDecoration),
                    static_cast<unsigned>(F::kNoBackground),
                    static_cast<unsigned>(F::kNoMove),
                    static_cast<unsigned>(F::kNoResize),
                    static_cast<unsigned>(F::kPassInputToGame),
                    static_cast<unsigned>(F::kHideHUD),
                    static_cast<unsigned>(F::kBlockVanity),
                    Settings::GetSingleton().performanceVanityCamera,
                    static_cast<unsigned>(F::kRenderDuringTM),
                    static_cast<unsigned>(F::kCloseOnGameMenu)));
            }
            void RenderOverlay() override {
                const auto& t = theme::Get();
                if (t.highwayBackground.empty()) return;
                auto* i = FUCK::GetInterface();
                if (!i) return;
                if (!_image && !_attempted) {
                    _attempted = true;
                    _image = i->LoadImage(t.highwayBackground.c_str(), false);
                    if (!_image) {
                        spdlog::warn("[theme] highway background could not be loaded: {}",
                                     t.highwayBackground);
                        return;
                    }
                    float w = 0.0f, h = 0.0f;
                    i->GetImageInfo(_image, &w, &h);
                    if (w > 0.0f && h > 0.0f && std::abs(w / h - 0.5f) > 0.01f) {
                        spdlog::warn("[theme] highway background is {:.0f}x{:.0f}; Clone Hero standard is 1:2",
                                     w, h);
                    }
                }
                if (!_image) return;

                const ImVec2 disp = FUCK::GetDisplaySize();
                if (disp.x <= 0.0f || disp.y <= 0.0f) return;
                const hw::View v{ disp.x, disp.y };
                const hw::Style st = hw::Style::Default();
                const float cx = v.w * 0.5f;
                const ImVec2 p0(cx - hw::HalfWOf(st, v, 1.0f), hw::YOf(st, v, 1.0f));
                const ImVec2 p1(cx + hw::HalfWOf(st, v, 1.0f), hw::YOf(st, v, 1.0f));
                const ImVec2 p2(cx + hw::HalfWOf(st, v, 0.0f), hw::YOf(st, v, 0.0f));
                const ImVec2 p3(cx - hw::HalfWOf(st, v, 0.0f), hw::YOf(st, v, 0.0f));
                i->DrawImageQuad(_image, p0, p1, p2, p3,
                                 ImVec2(0,0), ImVec2(1,0), ImVec2(1,1), ImVec2(0,1),
                                 t.highwayBackgroundTint);
            }
        private:
            void* _image = nullptr;
            bool _attempted = false;
        };

        ThemeHighwayBackground g_themeHighwayBackground;
    }

    void Register() {
        ApplyTheme();
        // Register the background first so FLICK composites the existing
        // highway surface/gems above it.
        FUCK::RegisterWindow(&g_themeHighwayBackground);
        RegisterHighwayWindow();
        RegisterHudWindow();
        RegisterResultsWindow();
        RegisterBrowserWindow();
        RegisterPauseMenuWindow();
        RegisterSettingsTool();
        spdlog::info("[render] M4 windows registered (theme={}, results kPassInputToGame=ON).",
                     theme::kThemeIniPath);
    }

    void AcquireCursor() {
        if (g_cursorRefs++ == 0) FUCK::ForceCursor(true);
    }
    void ReleaseCursor() {
        if (g_cursorRefs > 0 && --g_cursorRefs == 0) FUCK::ForceCursor(false);
    }
    int CursorRefs() { return g_cursorRefs; }
}
