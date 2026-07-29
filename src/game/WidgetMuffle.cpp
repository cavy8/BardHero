#include "PCH.h"
#include "game/WidgetMuffle.h"

#include "game/WidgetMuffleLogic.h"
#include "Settings.h"

#include "RE/G/GFxValue.h"
#include "RE/U/UI.h"

#include <SimpleIni.h>

#include <string>
#include <utility>
#include <vector>

namespace SH::WidgetMuffle {
    namespace {
        // Muffle v6 (2026-07-27), the surviving design after five field
        // rounds. The mechanism is a LADDER per menu, because the widget
        // mods differ in what they enforce and what they expose:
        //
        //   1. Invoke("widget.setVisible", ...) - STB's own AS config
        //      channel. Their Hooks.cpp re-asserts MOVIE-level visibility
        //      on every widget EVERY FRAME during play (self-healing,
        //      read from STB-Widgets-Nexus source), so movie-level hiding
        //      flickers (396 re-shows in a 16s song, measured) - but the
        //      AS layer is unenforced at runtime (a SetPlayTimeVisible=
        //      false widget stays hidden through hours of play).
        //   2. A known inner-clip path (shoutWidget: no widget.setVisible
        //      at all - field noapi probe) driven the way STB's own C++
        //      drives it, via SetVariable.
        //   3. Movie-level SetVisible for everything else (TrueHUD, and
        //      any menu a player adds to the INI list) - mods without a
        //      per-frame re-shower keep a movie-level hide.
        //
        // Restore re-applies each widget's own config (the mod's INI
        // visibility key) rather than blanket-true, so a config-hidden
        // widget is never force-shown at song end. THE CTD LESSON
        // (0xc0000374, isolated via Tests A/B 2026-07-27): those INI
        // reads happen on the SESSION thread in OnSessionStart, NEVER
        // inside a game-thread task - file IO in the restore task was the
        // prime suspect in the heap-corruption crash. The game-thread
        // tasks only touch engine objects and pre-resolved values.
        //
        // The per-invoke breadcrumb logs are the crash tripwire and stay
        // for one more field round (probe-removal precedent).

        enum class Mech { kAsApi, kInnerClip, kMovie };

        struct Hidden {
            std::string name;
            Mech        mech;
            bool        restoreVisible;  // the mod's own config, snapshot
        };

        // Game-thread-only, like BandStage's arrays: written and read
        // exclusively inside SKSE tasks.
        std::vector<Hidden> g_hidden;

        constexpr const char* kSetVisible = "widget.setVisible";

        // CTD 2026-07-28 (trainwreck, field): Invoke(kSetVisible) on the
        // goldWidget menu crashed inside Scaleform's call setup with a null
        // environment ([r14+38h], r14=0). GetMenu and uiMovie were both
        // valid - the movie object outlives its ActionScript state, and the
        // gold widget is exactly the widget that tears that state down on
        // its own schedule. So before CALLING into a movie's AS layer,
        // resolve the "widget" root object with GetVariable: a read that
        // returns false on a dead or half-initialized VM instead of
        // faulting (the same probe the shoutWidget "noapi" field run used
        // safely). A false here falls down the ladder to the movie-level
        // hide, which never enters the AS VM.
        [[nodiscard]] bool AsApiAlive(RE::GFxMovieView& a_movie) {
            RE::GFxValue root;
            return a_movie.GetVariable(&root, "widget") && root.IsObject();
        }

        // Does the clip we are about to write still EXIST? Same preflight
        // idea as AsApiAlive, for the path that never had one.
        //
        // AsApiAlive cannot serve here: it probes for a `widget` object, and
        // the inner-clip mechanism exists precisely for movies that have no
        // such object (shoutWidget is a bare timeline). So the inner-clip
        // writes went out with nothing checked but IsMenuOpen, and an open
        // menu is not the same claim as a live clip - a widget that rebuilt
        // its timeline leaves the path resolving to nothing, and SetVariable
        // then walks a dead object tree inside Scaleform.
        //
        // Probe the PARENT, never the leaf: "_root.shoutWidget._visible" is
        // a property, and a property of a live clip may legitimately be
        // absent while the clip itself is fine.
        [[nodiscard]] bool ClipAlive(RE::GFxMovieView& a_movie,
                                     std::string_view  a_clip) {
            const auto dot = a_clip.rfind('.');
            if (dot == std::string_view::npos) { return false; }
            RE::GFxValue parent;
            return a_movie.GetVariable(
                       &parent, std::string{ a_clip.substr(0, dot) }.c_str())
                   && parent.IsObject();
        }

        void Append(std::string& a_list, const std::string& a_name) {
            a_list += a_list.empty() ? a_name : ", " + a_name;
        }

        // SESSION THREAD ONLY (see the CTD lesson above): resolve every
        // configured menu's restore target from its mod's own INI. Missing
        // file or key -> visible, the right fallback everywhere.
        [[nodiscard]] std::vector<std::pair<std::string, bool>>
        ReadRestoreTargets() {
            const auto names = widget_muffle::ParseMenuList(
                Settings::GetSingleton().hideMenusDuringSession);
            std::vector<std::pair<std::string, bool>> out;
            out.reserve(names.size());
            CSimpleIniA inis[2];
            bool        loaded[2] = { false, false };
            for (const auto& name : names) {
                const auto rk = widget_muffle::RestoreKeyFor(name);
                bool       visible = true;
                if (!rk.ini.empty()) {
                    const int idx =
                        rk.ini == widget_muffle::kWidgetsIni ? 0 : 1;
                    if (!loaded[idx]) {
                        loaded[idx] = true;
                        inis[idx].SetUnicode();
                        const std::string path =
                            std::string{ "Data/SKSE/Plugins/" } +
                            std::string{ rk.ini };
                        inis[idx].LoadFile(path.c_str());
                    }
                    visible = inis[idx].GetBoolValue(
                        "Main", std::string{ rk.key }.c_str(), true);
                }
                out.emplace_back(name, visible);
            }
            return out;
        }

        void HideOnGameThread(
            const std::vector<std::pair<std::string, bool>>& a_targets) {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) { return; }
            std::string hid;
            std::string absent;
            int         viaApi = 0, viaClip = 0, viaMovie = 0;
            for (const auto& [name, restoreVisible] : a_targets) {
                auto menu = ui->GetMenu(name);
                // IsMenuOpen: GetMenu answers from the registry, which can
                // hold a menu whose movie is mid-construction or mid-
                // teardown; only a menu actually on the stack has a movie
                // that is safe to touch (goldWidget CTD, 2026-07-28).
                if (!menu || !menu->uiMovie || !ui->IsMenuOpen(name)) {
                    Append(absent, name);
                    continue;
                }
                auto& movie = *menu->uiMovie;
                // Crash tripwire: if the game dies inside a Flash call,
                // the last line names the movie and operation.
                spdlog::info("[muffle] invoke hide {}", name);
                Mech               mech;
                const RE::GFxValue off{ false };
                if (AsApiAlive(movie) &&
                    movie.Invoke(kSetVisible, nullptr, &off, 1)) {
                    mech = Mech::kAsApi;
                    ++viaApi;
                } else if (const auto clip =
                               widget_muffle::InnerClipFor(name);
                           !clip.empty() && ClipAlive(movie, clip) &&
                           movie.SetVariable(
                               std::string{ clip }.c_str(),
                               RE::GFxValue{ false })) {
                    mech = Mech::kInnerClip;
                    ++viaClip;
                } else {
                    movie.SetVisible(false);
                    mech = Mech::kMovie;
                    ++viaMovie;
                }
                g_hidden.push_back({ name, mech, restoreVisible });
                Append(hid, name);
            }
            // The absent list is the field probe: a name listed there on
            // a rig that RUNS the mod is a wrong menu name in the list.
            spdlog::info(
                "[muffle] hid [{}]; absent [{}] (api={} clip={} movie={})",
                hid.empty() ? "-" : hid, absent.empty() ? "-" : absent,
                viaApi, viaClip, viaMovie);
        }

        // Menu-close re-shows are the one thing that undoes a hide mid-
        // session: TrueHUD re-shows its movie when the pause menu closes
        // (field 2026-07-27 - "appears again and persists"), exactly like
        // STB's check2(true) except event-driven. The AS/clip entries
        // survive those events; the movie-level entries need this
        // re-assert, fired from the resume path AFTER the countdown so
        // the menu-close event has long since landed. Re-applying every
        // mechanism is harmless and keeps this one loop.
        void ReassertOnGameThread() {
            if (g_hidden.empty()) { return; }
            auto* ui = RE::UI::GetSingleton();
            if (!ui) { return; }
            int movieReshown = 0;
            for (const auto& h : g_hidden) {
                auto menu = ui->GetMenu(h.name);
                if (!menu || !menu->uiMovie || !ui->IsMenuOpen(h.name)) {
                    continue;
                }
                auto& movie = *menu->uiMovie;
                spdlog::info("[muffle] invoke rehide {}", h.name);
                switch (h.mech) {
                case Mech::kAsApi: {
                    // A dead AS layer here means the widget re-initialized
                    // from its own config; skipping is the correct state.
                    if (!AsApiAlive(movie)) { break; }
                    const RE::GFxValue off{ false };
                    movie.Invoke(kSetVisible, nullptr, &off, 1);
                    break;
                }
                case Mech::kInnerClip: {
                    const auto clip =
                        widget_muffle::InnerClipFor(h.name);
                    if (!clip.empty() && ClipAlive(movie, clip)) {
                        movie.SetVariable(
                            std::string{ clip }.c_str(),
                            RE::GFxValue{ false });
                    }
                    break;
                }
                case Mech::kMovie:
                    if (movie.GetVisible()) { ++movieReshown; }
                    movie.SetVisible(false);
                    break;
                }
            }
            // The count is the field probe: how many movie-level menus
            // the pause-close actually re-showed (expect TrueHUD = 1).
            spdlog::info(
                "[muffle] resume re-hide over {} menu(s) "
                "({} movie-level had re-shown)",
                g_hidden.size(), movieReshown);
        }

        void RestoreOnGameThread() {
            if (g_hidden.empty()) { return; }
            auto* ui = RE::UI::GetSingleton();
            if (!ui) { return; }
            std::size_t restored = 0;
            for (const auto& h : g_hidden) {
                auto menu = ui->GetMenu(h.name);
                if (!menu || !menu->uiMovie || !ui->IsMenuOpen(h.name)) {
                    continue;
                }
                auto& movie = *menu->uiMovie;
                spdlog::info("[muffle] invoke restore {}", h.name);
                switch (h.mech) {
                case Mech::kAsApi: {
                    // A dead AS layer means this instance's hide died with
                    // it; the next instance loads its own config, which IS
                    // the restore target. Skip, count it restored.
                    if (!AsApiAlive(movie)) {
                        ++restored;
                        break;
                    }
                    const RE::GFxValue vis{ h.restoreVisible };
                    if (movie.Invoke(kSetVisible, nullptr, &vis, 1)) {
                        ++restored;
                    }
                    break;
                }
                case Mech::kInnerClip: {
                    const auto clip =
                        widget_muffle::InnerClipFor(h.name);
                    // A clip that has gone took our hide with it, so the
                    // widget is already in whatever state its own config
                    // rebuilt it to. Count it restored; there is nothing
                    // left of ours to undo.
                    if (clip.empty() || !ClipAlive(movie, clip)) {
                        ++restored;
                        break;
                    }
                    if (movie.SetVariable(
                            std::string{ clip }.c_str(),
                            RE::GFxValue{ h.restoreVisible })) {
                        ++restored;
                    }
                    break;
                }
                case Mech::kMovie:
                    // Movie-level menus restore to visible; their own
                    // logic (TrueHUD's fades) rules from there.
                    movie.SetVisible(true);
                    ++restored;
                    break;
                }
            }
            spdlog::info("[muffle] restored {}/{} widget menu(s)",
                         restored, g_hidden.size());
            g_hidden.clear();
        }
    }

    void OnSessionStart() {
        // Session thread: INI reads here, engine writes in the task.
        auto targets = ReadRestoreTargets();
        if (targets.empty()) { return; }
        SKSE::GetTaskInterface()->AddTask(
            [targets = std::move(targets)] {
                HideOnGameThread(targets);
            });
    }

    void OnSessionResume() {
        SKSE::GetTaskInterface()->AddTask([] { ReassertOnGameThread(); });
    }

    void OnSessionEnd() {
        SKSE::GetTaskInterface()->AddTask([] { RestoreOnGameThread(); });
    }
}
