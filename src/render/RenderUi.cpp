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

        // ---- highway background tiling --------------------------------
        // One tile of the image spans exactly one lookahead of chart, so a
        // grain line covers the highway in the same time a note does - the
        // texture and the notes it carries move as one surface.
        constexpr float kBgTiles  = 1.0f;
        // Strip count: a trapezoid is two triangles with AFFINE uv, so the
        // texture's vertical lines kink across the diagonal by up to half
        // the strip's width change. At 64 strips that residue is ~2px on a
        // 1080p highway; as one quad it was ~130px (field 2026-08-19).
        constexpr int   kBgStrips = 64;
        // Depth cuts (kBgStrips + 1 boundaries) plus room for the tile
        // seams merged in among them.
        constexpr int   kBgMaxCuts = kBgStrips + 5;

        // Draws the highway background as depth strips whose v is a
        // function of SONG TIME rather than of screen depth. Screen-depth v
        // is what a single quad gives, and it is wrong twice over: it has
        // no perspective foreshortening, and it cannot scroll at all.
        void DrawTiledHighway(void* image, const ImVec4& tint,
                              const hw::Style& st, const hw::View& v,
                              double visual, double lookahead) {
            auto* i = FUCK::GetInterface();
            if (!i || !image || lookahead <= 0.0) return;

            // Tile phase at the strikeline. v runs BACKWARDS along u so the
            // image's bottom edge sits at the strikeline, as the untiled
            // draw had it; anchoring it to visual time is what pins the
            // texture to the chart instead of to the camera.
            float vBase = static_cast<float>(
                std::fmod(visual / lookahead * kBgTiles, 1.0));
            if (vBase < 0.0f) vBase += 1.0f;

            // Depth cuts, plus a cut at every tile seam: FLICK makes no
            // promise about a repeating sampler, so the wrap is done here
            // in geometry and every quad keeps its uv inside [0,1].
            float us[kBgMaxCuts];
            int   n = 0;
            for (int c = 0; c <= kBgStrips; ++c) {
                us[n++] = hw::UOfZ(static_cast<float>(c) / kBgStrips,
                                   st.depthGain);
            }
            for (int m = 1;
                 m <= static_cast<int>(std::ceil(kBgTiles)) &&
                 n < kBgMaxCuts;
                 ++m) {
                const float u = (static_cast<float>(m) - vBase) / kBgTiles;
                if (u > 0.0f && u < 1.0f) us[n++] = u;
            }
            std::sort(us, us + n);

            const float cx = v.w * 0.5f;
            for (int c = 0; c + 1 < n; ++c) {
                const float u0 = us[c], u1 = us[c + 1];  // near, far
                if (u1 - u0 <= 1e-6f) continue;
                const float z0 = hw::ZOf(u0, st.depthGain);
                const float z1 = hw::ZOf(u1, st.depthGain);
                const float y0 = hw::YOf(st, v, z0);
                const float y1 = hw::YOf(st, v, z1);
                const float h0 = hw::HalfWOf(st, v, z0);
                const float h1 = hw::HalfWOf(st, v, z1);
                // No seam lies strictly inside the span, so both ends share
                // a tile index; take it from the midpoint and subtract.
                const float raw0 = -(vBase + u0 * kBgTiles);
                const float raw1 = -(vBase + u1 * kBgTiles);
                const float tile = std::floor((raw0 + raw1) * 0.5f);
                const float t0 = std::clamp(raw0 - tile, 0.0f, 1.0f);
                const float t1 = std::clamp(raw1 - tile, 0.0f, 1.0f);
                i->DrawImageQuad(image,
                                 ImVec2(cx - h1, y1), ImVec2(cx + h1, y1),
                                 ImVec2(cx + h0, y0), ImVec2(cx - h0, y0),
                                 ImVec2(0.0f, t1), ImVec2(1.0f, t1),
                                 ImVec2(1.0f, t0), ImVec2(0.0f, t0), tint);
            }
        }

        // Visual (song-domain) time driving the background scroll. A live
        // session owns the clock; the theme editor's preview has none, so
        // any monotonic clock scrolls at the right RATE - which is all the
        // preview is showing.
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

                if (!_image && !_attempted) {
                    _attempted = true;
                    _loadedPath = t.highwayBackground;
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

                const ImVec2 disp = FUCK::GetDisplaySize();
                if (disp.x <= 0.0f || disp.y <= 0.0f) return;
                const hw::View  v{ disp.x, disp.y };
                const hw::Style st = hw::Style::Default();
                // Same clamp as the highway itself: the two share a
                // lookahead or the surface slides under its own notes.
                const double lookahead = std::clamp(
                    Settings::GetSingleton().highwayLookaheadSec, 0.3, 5.0);
                DrawTiledHighway(_image, t.highwayBackgroundTint, st, v,
                                 BackgroundScrollTime(), lookahead);
            }

        private:
            void* _image = nullptr;
            bool _attempted = false;
            std::string _loadedPath;
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
