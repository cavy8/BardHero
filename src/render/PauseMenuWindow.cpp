// src/render/PauseMenuWindow.cpp
#include "PCH.h"
#include "render/PauseMenuWindow.h"

#include "game/Session.h"
#include "game/ListNavigationLogic.h"
#include "game/UiBus.h"
#include "render/RenderUi.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"
#include "render/PanelStyle.h"
#include "render/PauseLayout.h"
#include "render/UiSound.h"

#include <algorithm>

namespace SH {
    namespace {
        class PauseMenuWindow final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "SessionPauseV5"; }
            const char* Title() const override { return "Paused"; }
            bool        IsOpen() const override {
                const bool open =
                    UiBus::GetSingleton().pauseMenuOpen.load();
                // the session thread closes this by flipping the flag
                // (resume/quit/load) - Draw then stops being called, so
                // the cursor must be released HERE (browser force-close
                // precedent)
                if (!open) {
                    const_cast<PauseMenuWindow*>(this)->_drawHeld = false;
                    if (_mouseCursorHeld) {
                        RenderUi::ReleaseCursor();
                        const_cast<PauseMenuWindow*>(this)->_mouseCursorHeld =
                            false;
                    }
                }
                return open;
            }
            void SetOpen(bool) override {
                // session-thread owned state; the host's close paths
                // (kCloseOnGameMenu) only HIDE the window
            }
            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                // no kCloseOnEsc: InputHook owns Escape as a direct resume
                // toggle while this custom pause surface is active.
                return static_cast<F>(
                    static_cast<unsigned>(F::kNoDecoration) |
                    static_cast<unsigned>(F::kNoBackground) |
                    static_cast<unsigned>(F::kNoMove) |
                    static_cast<unsigned>(F::kNoResize) |
                    static_cast<unsigned>(F::kHideHUD) |
                    static_cast<unsigned>(F::kCloseOnGameMenu));
            }
            ImVec2 GetDefaultSize() const override {
                // SCALED. The layout constants are logical, and every piece
                // of content below is drawn at `* z` - so returning them raw
                // sized the window for scale 1.0 while filling it with
                // content sized for the real scale. At any scale above 1.0
                // the last row fell outside the bounded content and was
                // clipped (field 2026-07-26: the Quit button, once a fourth
                // row existed to expose it). Sibling panels get this right by
                // deriving from GetDisplaySize; this one had no scale term at
                // all.
                const float z = FUCK::Scale(1.0f);
                // Measured height once the panel has drawn a settled frame;
                // the constant only sizes the very first one. kNoMove |
                // kNoResize means the host re-reads this every frame, so the
                // correction lands immediately and then holds.
                if (_measuredContent > 0.0f) {
                    return ImVec2(pause_layout::kDefaultWidth * z,
                                  pause_layout::WindowHeightForContent(
                                      _measuredContent, z));
                }
                return ImVec2(pause_layout::kDefaultWidth * z,
                              pause_layout::kDefaultHeight * z);
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                return ImVec2((d.x - s.x) * 0.5f, (d.y - s.y) * 0.5f);
            }

            void Draw() override {
                auto& bus = UiBus::GetSingleton();
                if (!_drawHeld) {
                    _drawHeld = true;
                    _sel      = 0;  // default = Resume (one-press Plus)
                    RenderUi::AcquireCursor();
                    _mouseCursorHeld = true;
                    bus.DrainNav();
                    _navRepeat.Reset();
                    _enterAt = FUCK::GetTime();  // P6 content slam-in
                }
                const int navMove = _navRepeat.Step(
                    bus.navMove.exchange(0), bus.navHeldMove.load(),
                    FUCK::GetTime());
                const bool confirm = bus.navConfirm.exchange(false);
                const int before = _sel;
                if (navMove != 0) {
                    _sel = list_navigation::WrapIndex(
                        _sel, navMove, pause_layout::kButtonCount);
                }
                if (_sel != before) {
                    ui_sound::Play(ui_sound::Event::kFocus);
                }

                panel::DrawCurrent(panel::kAccent, 0.86f,
                                   panel_style::PauseSurface());
                const int childFlags =
                    pause_layout::SuppressScrollbar()
                        ? static_cast<int>(ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoScrollWithMouse)
                        : 0;
                panel::BeginBoundedContent("##pause_content",
                                           pause_layout::kContentInset,
                                           childFlags);
                // P6: content slam-in - the window cannot move in-Draw
                // (FLICK no-op), so a shrinking spacer slides the content
                // up into place over ~0.22s
                {
                    const float et = std::clamp(
                        static_cast<float>(
                            (FUCK::GetTime() - _enterAt) / 0.22),
                        0.0f, 1.0f);
                    const float ee = et * et * (3.0f - 2.0f * et);
                    if (ee < 1.0f) {
                        FUCK::Dummy(ImVec2(
                            1.0f,
                            (1.0f - ee) * 18.0f * FUCK::Scale(1.0f)));
                    }
                    // Gates the self-measurement below: while this spacer is
                    // still shrinking the content height changes every frame.
                    _slamSettled = ee >= 1.0f;
                }
                panel::Header("PAUSED");
                const float z = FUCK::Scale(1.0f);
                FUCK::Dummy(
                    ImVec2(1.0f, pause_layout::kTopGap * z));
                panel::PushControls();
                FUCK::PushStyleVar(ImGuiStyleVar_SelectableTextAlign,
                                   ImVec2(0.5f, 0.5f));
                // Practice reads its own state so the row can say what
                // confirming it will DO, rather than naming a mode the
                // player then has to guess the direction of.
                const bool inPractice =
                    bus.practiceActive.load(std::memory_order_acquire);
                const char* rows[pause_layout::kButtonCount] = {
                    "Resume", "Restart",
                    inPractice ? "Leave practice" : "Practice this song",
                    "Settings", "Quit"
                };
                int chosen = -1;
                for (int i = 0; i < pause_layout::kButtonCount; ++i) {
                    if (i > 0) {
                        FUCK::Dummy(
                            ImVec2(1.0f, pause_layout::kButtonGap * z));
                    }
                    if (FUCK::Selectable(
                            rows[i], _sel == i, 0,
                            ImVec2(0, pause_layout::kButtonHeight * z))) {
                        _sel   = i;
                        chosen = i;
                    }
                }
                FUCK::PopStyleVar();
                if (confirm) { chosen = _sel; }
                panel::PopControls();
                // THE MEASUREMENT. The layout cursor sits where the next
                // element would start, so this is the full height the rows
                // above actually consumed - ImGui's ItemSpacing and the
                // header's real text extent included, which is exactly what
                // the constants could not model.
                //
                // Only while the slam-in has finished: its shrinking spacer
                // changes the height every frame for 0.22s, and feeding that
                // back would resize the window throughout the animation.
                if (_slamSettled) {
                    const float drawn = FUCK::GetCursorPos().y;
                    if (drawn > 1.0f && drawn != _measuredContent) {
                        if (_measuredContent <= 0.0f) {
                            spdlog::info(
                                "[pause] content measured at {:.0f}px - "
                                "panel sized to fit (seed was {:.0f})",
                                drawn,
                                (pause_layout::kDefaultHeight -
                                 pause_layout::kHostVerticalBudget -
                                 2.0f * pause_layout::kContentInset) *
                                    FUCK::Scale(1.0f));
                        }
                        _measuredContent = drawn;
                    }
                }
                panel::EndBoundedContent();
                if (chosen >= 0) {
                    ui_sound::Play(ui_sound::Event::kConfirm);
                    switch (chosen) {
                        case 0: Session::RequestResume(); break;
                        case 1: Session::RequestRestart(); break;
                        case 2: Session::RequestPracticeToggle(); break;
                        case 3:
                            // Opens FLICK's own menu, where BardHero's page
                            // lives. Deliberately does NOT resume or close
                            // this panel: the player came here to change a
                            // setting and then carry on, and the world stays
                            // frozen meanwhile. FLICK draws over us.
                            //
                            // This is the DISCOVERABLE route. The direct one
                            // (FLICK's own hotkey) works during a song too
                            // now that an open FLICK menu releases the
                            // keyboard, but a hotkey nobody remembers is not
                            // a feature.
                            FUCK::SetMenuOpen(true);
                            break;
                        default: Session::RequestAbort(); break;
                    }
                }
            }

        private:
            int  _sel = 0;
            list_navigation::HeldRepeat _navRepeat;
            bool _drawHeld = false, _mouseCursorHeld = false;
            double _enterAt = 0.0;
            // Self-sizing (see PauseLayout.h). Deliberately NOT reset with
            // _drawHeld: the measurement describes the ROW SET, which does
            // not change between opens, so carrying it across means the
            // seed height is only ever seen on the first pause of a session
            // rather than on every one.
            bool  _slamSettled     = false;
            float _measuredContent = 0.0f;
        };
        PauseMenuWindow g_pauseMenu;
    }

    void RegisterPauseMenuWindow() { FUCK::RegisterWindow(&g_pauseMenu); }
}
