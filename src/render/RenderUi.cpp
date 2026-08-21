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
#include "render/HighwayFullscreenLayerD3D.h"

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

        // Use FLICK's textured quad API; borrowing another ImGui context
        // is unsafe.
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

        // Fit and center a full-screen image without stretching.
        void DrawScaledImage(void* image, const ImVec4& tint,
                             const hw::View& v) {
            auto* i = FUCK::GetInterface();
            if (!i || !image) return;
            float texW = 0.0f, texH = 0.0f;
            i->GetImageInfo(image, &texW, &texH);
            if (texW <= 0.0f || texH <= 0.0f) return;
            const float scale = std::min(v.w / texW, v.h / texH);
            const float drawW = texW * scale;
            const float drawH = texH * scale;
            const float x0 = (v.w - drawW) * 0.5f;
            const float y0 = (v.h - drawH) * 0.5f;
            i->DrawImageQuad(
                image, ImVec2(x0, y0), ImVec2(x0 + drawW, y0),
                ImVec2(x0 + drawW, y0 + drawH), ImVec2(x0, y0 + drawH),
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f),
                ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f), tint);
        }

        FUCK::WindowFlags HighwayLayerFlags() {
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

        bool HighwayLayerShouldShow() {
            return EngineFeed::GetSingleton().active.load(
                       std::memory_order_acquire) ||
                   theme_preview::Is(theme_preview::Target::kHighway);
        }

        // Underlay and midlayer share D3D rendering with an image-quad fallback.
        class FullscreenLayerLogic {
        public:
            void Refresh() {
                if (_image) {
                    if (auto* i = FUCK::GetInterface()) {
                        i->ReleaseImage(_image);
                    }
                }
                _image = nullptr;
                _attempted = false;
                _loadedPath.clear();
                _surface.Refresh();
            }

            void Render(const std::string& path, const ImVec4& tint,
                       const hw::View& v) {
                if (path.empty()) return;
                auto* i = FUCK::GetInterface();
                if (!i) return;
                if (_loadedPath != path) Refresh();
                _loadedPath = path;
                if (_surface.Render(path, ToRgba(tint), v)) return;

                if (!_image && !_attempted) {
                    _attempted = true;
                    _image = i->LoadImage(path.c_str(), false);
                    if (!_image) {
                        spdlog::warn(
                            "[theme] highway layer image could not be "
                            "loaded: {}",
                            path);
                        return;
                    }
                }
                if (!_image) return;
                DrawScaledImage(_image, tint, v);
            }

        private:
            hw::HighwayFullscreenLayerD3D _surface;
            void* _image = nullptr;
            bool _attempted = false;
            std::string _loadedPath;
        };

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
                // Preview the highway on the same background used in gameplay.
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

                // Refresh if the mutable path changed without an explicit request.
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

        // Below everything, including the highway background image itself
        // (see Register(): this window is registered before it, and both
        // draw via manual D3D, so this one's draw call always lands
        // first and gets painted over).
        class ThemeHighwayUnderlay final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "ThemeHighwayUnderlay"; }
            const char* Title() const override {
                return "BardHero Highway Underlay";
            }
            bool IsOpen() const override {
                return !theme::Get().highwayUnderlay.empty() &&
                       HighwayLayerShouldShow();
            }
            void SetOpen(bool) override {}
            void Draw() override {}
            FUCK::WindowFlags GetFlags() const override {
                return HighwayLayerFlags();
            }

            void Refresh() { _logic.Refresh(); }

            void RenderOverlay() override {
                const auto& t = theme::Get();
                if (t.highwayUnderlay.empty()) return;
                const ImVec2 disp = FUCK::GetDisplaySize();
                if (disp.x <= 0.0f || disp.y <= 0.0f) return;
                _logic.Render(t.highwayUnderlay, t.highwayUnderlayTint,
                             hw::View{ disp.x, disp.y });
            }

        private:
            FullscreenLayerLogic _logic;
        };
        ThemeHighwayUnderlay g_themeHighwayUnderlay;

        // Between FLICK's own rendering (the highway's gems/trails/HUD/
        // banners, all drawn through FUCK's DrawImageQuad) and the highway
        // background image: registered after that background, so its
        // manual D3D draw paints over it, but - like every manual D3D
        // draw issued from RenderOverlay() - it still lands before FLICK's
        // ImGui frame for this frame is flushed, which is what keeps it
        // under all of that ImGui-drawn highway content.
        class ThemeHighwayMidlayer final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "ThemeHighwayMidlayer"; }
            const char* Title() const override {
                return "BardHero Highway Midlayer";
            }
            bool IsOpen() const override {
                return !theme::Get().highwayMidlayer.empty() &&
                       HighwayLayerShouldShow();
            }
            void SetOpen(bool) override {}
            void Draw() override {}
            FUCK::WindowFlags GetFlags() const override {
                return HighwayLayerFlags();
            }

            void Refresh() { _logic.Refresh(); }

            void RenderOverlay() override {
                const auto& t = theme::Get();
                if (t.highwayMidlayer.empty()) return;
                const ImVec2 disp = FUCK::GetDisplaySize();
                if (disp.x <= 0.0f || disp.y <= 0.0f) return;
                _logic.Render(t.highwayMidlayer, t.highwayMidlayerTint,
                             hw::View{ disp.x, disp.y });
            }

        private:
            FullscreenLayerLogic _logic;
        };
        ThemeHighwayMidlayer g_themeHighwayMidlayer;

        // The overlay must sit above BardHero's ImGui highway content, so it
        // uses FLICK's image quad and is registered last.
        class ThemeHighwayOverlayTop final : public FUCK::IWindow {
        public:
            const char* Id() const override {
                return "ThemeHighwayOverlayTop";
            }
            const char* Title() const override {
                return "BardHero Highway Overlay";
            }
            bool IsOpen() const override {
                return !theme::Get().highwayOverlay.empty() &&
                       HighwayLayerShouldShow();
            }
            void SetOpen(bool) override {}
            void Draw() override {}
            FUCK::WindowFlags GetFlags() const override {
                return HighwayLayerFlags();
            }

            void Refresh() {
                if (_image) {
                    if (auto* i = FUCK::GetInterface()) {
                        i->ReleaseImage(_image);
                    }
                }
                _image = nullptr;
                _attempted = false;
                _loadedPath.clear();
            }

            void RenderOverlay() override {
                const auto& t = theme::Get();
                if (t.highwayOverlay.empty()) return;
                auto* i = FUCK::GetInterface();
                if (!i) return;
                if (_loadedPath != t.highwayOverlay) Refresh();
                _loadedPath = t.highwayOverlay;

                if (!_image && !_attempted) {
                    _attempted = true;
                    _image = i->LoadImage(t.highwayOverlay.c_str(), false);
                    if (!_image) {
                        spdlog::warn(
                            "[theme] highway overlay image could not be "
                            "loaded: {}",
                            t.highwayOverlay);
                        return;
                    }
                }
                if (!_image) return;

                const ImVec2 disp = FUCK::GetDisplaySize();
                if (disp.x <= 0.0f || disp.y <= 0.0f) return;
                DrawScaledImage(_image, t.highwayOverlayTint,
                               hw::View{ disp.x, disp.y });
            }

        private:
            void* _image = nullptr;
            bool _attempted = false;
            std::string _loadedPath;
        };
        ThemeHighwayOverlayTop g_themeHighwayOverlayTop;
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
        if (reloadHighwayImage) {
            g_themeHighwayBackground.Refresh();
            g_themeHighwayUnderlay.Refresh();
            g_themeHighwayMidlayer.Refresh();
            g_themeHighwayOverlayTop.Refresh();
        }
    }

    void Register() {
        ApplyTheme(true);
        // Registration order: underlay, background, midlayer, highway content,
        // panels, then overlay.
        FUCK::RegisterWindow(&g_themeHighwayUnderlay);
        FUCK::RegisterWindow(&g_themeHighwayBackground);
        FUCK::RegisterWindow(&g_themeHighwayMidlayer);
        RegisterHighwayWindow();
        RegisterHudWindow();
        RegisterResultsWindow();
        RegisterBrowserWindow();
        RegisterPauseMenuWindow();
        RegisterSettingsTool();
        RegisterThemeTool();
        FUCK::RegisterWindow(&g_themeHighwayOverlayTop);
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
