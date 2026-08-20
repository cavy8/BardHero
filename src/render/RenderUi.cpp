// src/render/RenderUi.cpp
#include "PCH.h"
#include "render/RenderUi.h"

#include "QpcClock.h"
#include "Settings.h"
#include "clock/MasterClock.h"
#include "game/EngineFeed.h"
#include "render/BrowserWindow.h"
#include "render/HighwayLayout.h"
#include "render/HighwayWindow.h"
#include "render/HudWindow.h"
#include "render/PauseMenuWindow.h"
#include "render/ResultsWindow.h"
#include "render/SettingsTool.h"
#include "render/Theme.h"
#include "render/ThemePreview.h"
#include "render/ThemeTool.h"
#include "render/FlickWindowPolicy.h"
#include "render/HighwaySurfaceD3D.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"
#include "render/PanelStyle.h"

#include <algorithm>
#include <cmath>

namespace SH::RenderUi {
    namespace {
        int g_cursorRefs = 0;  // render thread only

        hw::RGBA ToRgba(const ImVec4& c) {
            return { c.x, c.y, c.z, c.w };
        }

        // FLICK currently exposes a whole textured quad, but not its active
        // ImDrawList or a textured-mesh primitive. Keep this on the supported
        // renderer API: borrowing another overlay's ImGui context crashes.
        void DrawHighwayBackground(void* image, const ImVec4& tint,
                                   const hw::Style& st, const hw::View& v,
                                   double visual, double lookahead) {
            auto* i = FUCK::GetInterface();
            if (!i || !image || lookahead <= 0.0) return;

            const float cx    = v.w * 0.5f;
            const float farY  = hw::YOf(st, v, 1.0f);
            const float nearY = hw::YOf(st, v, 0.0f);
            const float farW  = hw::HalfWOf(st, v, 1.0f);
            const float nearW = hw::HalfWOf(st, v, 0.0f);
            float phase = static_cast<float>(
                std::fmod(visual / lookahead, 1.0));
            if (phase < 0.0f) phase += 1.0f;
            i->DrawImageQuad(
                image,
                ImVec2(cx - farW, farY), ImVec2(cx + farW, farY),
                ImVec2(cx + nearW, nearY), ImVec2(cx - nearW, nearY),
                ImVec2(0.0f, -phase), ImVec2(1.0f, -phase),
                ImVec2(1.0f, 1.0f - phase),
                ImVec2(0.0f, 1.0f - phase), tint);
        }

        double BackgroundScrollTime() {
            auto& feed = EngineFeed::GetSingleton();
            if (feed.active.load(std::memory_order_acquire)) {
                std::scoped_lock lk(feed.mx);
                if (feed.clock) return feed.clock->VisualTime(QpcSec());
            }
            return QpcSec();
        }

        class ThemeHighwayBackground final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "ThemeHighwayBackground"; }
            const char* Title() const override {
                return "BardHero Highway Background";
            }
            bool IsOpen() const override {
                if (theme::Get().highwayBackground.empty()) return false;
                // The theme editor's highway preview is composited on the
                // real background for the same reason it uses the real
                // trapezoid: a tint judged against a blank surface is a
                // tint judged against something no player will ever see.
                return EngineFeed::GetSingleton().active.load(
                           std::memory_order_acquire) ||
                       theme_preview::Is(theme_preview::Target::kHighway);
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

            void Refresh() {
                if (_image) {
                    if (auto* i = FUCK::GetInterface()) i->ReleaseImage(_image);
                }
                _image = nullptr;
                _attempted = false;
                _loadedPath.clear();
                _surface.Refresh();
            }

            void RenderOverlay() override {
                const auto& t = theme::Get();
                if (t.highwayBackground.empty()) return;
                auto* i = FUCK::GetInterface();
                if (!i) return;

                // If a caller changed the mutable path but forgot to request
                // an explicit refresh, fail safe by noticing it here. The
                // editor normally refreshes on field commit, so this branch
                // costs only a string compare per rendered frame.
                if (_loadedPath != t.highwayBackground) Refresh();

                const ImVec2 disp = FUCK::GetDisplaySize();
                if (disp.x <= 0.0f || disp.y <= 0.0f) return;
                const hw::View  v{ disp.x, disp.y };
                const hw::Style st = hw::Style::Default();
                const double lookahead = std::clamp(
                    Settings::GetSingleton().highwayLookaheadSec, 0.3, 5.0);
                const double visual = BackgroundScrollTime();
                _loadedPath = t.highwayBackground;
                if (_surface.RenderBackground(
                        t.highwayBackground, ToRgba(t.highwayBackgroundTint),
                        st, v, visual, lookahead)) {
                    return;
                }

                if (!_image && !_attempted) {
                    _attempted = true;
                    _image = i->LoadImage(t.highwayBackground.c_str(), false);
                    if (!_image) {
                        spdlog::warn(
                            "[theme] highway background could not be loaded: {}",
                            t.highwayBackground);
                        return;
                    }
                    float w = 0.0f, h = 0.0f;
                    i->GetImageInfo(_image, &w, &h);
                    if (w > 0.0f && h > 0.0f &&
                        std::abs(w / h - 0.5f) > 0.01f) {
                        spdlog::warn(
                            "[theme] highway background is {:.0f}x{:.0f}; "
                            "Clone Hero standard is 1:2",
                            w, h);
                    }
                }
                if (!_image) return;

                DrawHighwayBackground(
                    _image, t.highwayBackgroundTint, st, v,
                    visual, lookahead);
            }

        private:
            void* _image = nullptr;
            bool _attempted = false;
            std::string _loadedPath;
            hw::HighwaySurfaceD3D _surface;
        };

        ThemeHighwayBackground g_themeHighwayBackground;
    }

    void ApplyTheme(bool reloadHighwayImage) {
        const auto& t = theme::Get();
        panel::ApplyTheme(t);
        for (int i = 0; i < 5; ++i) {
            hw::kLaneColors[i] = ToRgba(t.fret[i]);
        }
        hw::kOpenColor    = ToRgba(t.openNote);
        hw::kMissGrey     = ToRgba(t.miss);
        hw::kSpActiveCyan = ToRgba(t.starPower);
        hw::kHighwayGradient = ToRgba(t.highwayGradient);
        hw::kHighwayBorderLine = ToRgba(t.highwayBorderLine);
        hw::kHighwayStrikeline = ToRgba(t.highwayStrikeline);
        hw::kHighwayMeasureLine = ToRgba(t.highwayMeasureLine);
        if (reloadHighwayImage) g_themeHighwayBackground.Refresh();
    }

    void Register() {
        ApplyTheme(true);
        // Register the background first so FLICK composites the existing
        // highway surface/gems above it.
        FUCK::RegisterWindow(&g_themeHighwayBackground);
        RegisterHighwayWindow();
        RegisterHudWindow();
        RegisterResultsWindow();
        RegisterBrowserWindow();
        RegisterPauseMenuWindow();
        RegisterSettingsTool();
        RegisterThemeTool();
        spdlog::info(
            "[render] M4 windows registered (theme={}, live editor=ON, "
            "results kPassInputToGame=ON).",
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
