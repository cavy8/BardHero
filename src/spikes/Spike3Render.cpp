#include "PCH.h"
#include "Spike3Render.h"

#include "Branding.h"
#include "Settings.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace SH::Spike3 {
    namespace {
        constexpr int kQuads = 300;

        class SpikeWindow : public FUCK::IWindow {
        public:
            const char* Id() const override { return "SpikeHighway"; }
            const char* Title() const override { return "BardHero Spike"; }
            bool        IsOpen() const override {
                return Settings::GetSingleton().spike3Render;
            }
            void SetOpen(bool) override {}
            void Draw() override {}
            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                // kPassInputToGame is load-bearing: without it FLICK blocks
                // player control while ANY window is open, and this one is
                // open for the whole session (field-found: main menu clicks
                // dead, only the console reachable).
                return static_cast<F>(
                    static_cast<unsigned>(F::kNoDecoration) |
                    static_cast<unsigned>(F::kNoBackground) |
                    static_cast<unsigned>(F::kNoMove) |
                    static_cast<unsigned>(F::kPassInputToGame) |
                    static_cast<unsigned>(F::kRenderDuringTM));
            }

            // All drawing here: FLICK's fullscreen transparent overlay layer
            // (this is exactly where the M4 highway will live). FLICK's ImGui
            // context is PRIVATE - FUCK:: calls only, no ImGui:: (house rule).
            void RenderOverlay() override {
                if (!_atlas.IsLoaded()) {
                    _atlas = FUCK::Image(
                        "Data/SKSE/Plugins/BardHero/spike/atlas.png");
                    if (!_atlas.IsLoaded()) {
                        return;
                    }
                }
                const ImVec2 disp = FUCK::GetDisplaySize();
                const float  sw = disp.x, sh = disp.y;
                if (sw <= 0.0f || sh <= 0.0f) {
                    return;
                }

                LARGE_INTEGER f, t0, t1;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&t0);

                // Scrolling trapezoid strip grid: 5 lanes x 60 rows, the quad
                // shape the real highway needs (4 free corners + cell UVs).
                // Time-based scroll (QPC delta): per-frame increments were
                // visibly frame-rate coupled the moment console/alt-tab
                // changed frame pacing (field-found). The real highway is
                // time-linear by spec section 9; the spike models that too.
                {
                    LARGE_INTEGER qf, qn;
                    QueryPerformanceFrequency(&qf);
                    QueryPerformanceCounter(&qn);
                    const double tNow = static_cast<double>(qn.QuadPart) /
                                        static_cast<double>(qf.QuadPart);
                    if (_lastTime > 0.0) {
                        const double dt =
                            std::clamp(tNow - _lastTime, 0.0, 0.1);
                        _scroll += static_cast<float>(300.0 * dt);  // px/s
                    }
                    _lastTime = tNow;
                }
                for (int i = 0; i < kQuads; ++i) {
                    const int   lane = i % 5;
                    const int   row  = i / 5;
                    const float v =
                        std::fmod(_scroll + row * 14.0f, 840.0f) / 840.0f;
                    const float yTop = sh * (0.15f + 0.75f * v);
                    const float yBot = yTop + 12.0f;
                    // horizon pinch: lanes converge toward the top
                    auto laneX = [&](float y, int l) {
                        const float t     = (y - sh * 0.15f) / (sh * 0.75f);
                        const float width = 60.0f + 240.0f * t;
                        return sw * 0.5f + (l - 2) * width * 0.45f;
                    };
                    const float cu = (i % 4) * 0.25f;
                    const float cv = ((i / 4) % 4) * 0.25f;
                    FUCK::DrawImageQuad(
                        _atlas.GetID(),
                        ImVec2(laneX(yTop, lane) - 14.f, yTop),
                        ImVec2(laneX(yTop, lane) + 14.f, yTop),
                        ImVec2(laneX(yBot, lane) + 16.f, yBot),
                        ImVec2(laneX(yBot, lane) - 16.f, yBot),
                        ImVec2(cu, cv), ImVec2(cu + 0.25f, cv),
                        ImVec2(cu + 0.25f, cv + 0.25f), ImVec2(cu, cv + 0.25f),
                        ImVec4(1, 1, 1, 0.9f));
                }

                QueryPerformanceCounter(&t1);
                const double us = (t1.QuadPart - t0.QuadPart) * 1e6 /
                                  static_cast<double>(f.QuadPart);
                _sumUs += us;
                _maxUs = std::max(_maxUs, us);
                if (++_frames % 300 == 0) {
                    spdlog::info(
                        "[SPIKE-3] {} quads: mean={:.1f}us max={:.1f}us over 300 frames",
                        kQuads, _sumUs / 300.0, _maxUs);
                    _sumUs = 0.0;
                    _maxUs = 0.0;
                }
            }

        private:
            FUCK::Image   _atlas;
            float         _scroll   = 0.0f;
            double        _lastTime = 0.0;
            double        _sumUs  = 0.0;
            double        _maxUs  = 0.0;
            std::uint64_t _frames = 0;
        };
        SpikeWindow g_window;
    }

    void Register() {
        if (!Settings::GetSingleton().spike3Render) {
            return;
        }
        FUCK::RegisterWindow(&g_window);
        spdlog::info("[SPIKE-3] window registered.");
    }
}
