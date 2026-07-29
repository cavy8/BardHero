// src/render/BrowserWindow.cpp
#include "PCH.h"
#include "render/BrowserWindow.h"

#include "Branding.h"
#include "Settings.h"
#include "game/EngineFeed.h"
#include "game/ListNavigationLogic.h"
#include "game/ResultsLogic.h"
#include "game/SgtProgression.h"  // the shared rank - see the gate in Draw()
#include "game/SongEligibility.h"
#include "game/SongLibrary.h"
#include "chart/LoadSong.h"  // bard::ResolveDifficulty
#include "practice/PracticeRange.h"  // ResolveRange - the picker DISPLAYS it
#include "util/PathText.h"
#include "game/StarLedger.h"
#include "game/StarsLogic.h"
#include "game/UiBus.h"
#include "game/UnlockLogic.h"
#include "render/RenderUi.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"
#include "render/PanelStyle.h"
#include "render/PracticeLayout.h"
#include "render/SongSortLogic.h"
#include "render/UiSound.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace SH {
    namespace {
        // A locked row is DIMMED, never hidden (spec 6.4): a locked chart
        // advertises the progression, a hidden one just looks like a missing
        // file. Muted rather than invisible - the title still has to read.
        constexpr ImVec4 kLockedRow{ 0.56f, 0.53f, 0.49f, 1.0f };
        // footer note under Play; desaturated unless a refusal flashes red
        constexpr ImVec4 kLockedNote{ 0.70f, 0.70f, 0.66f, 1.0f };
        // ... and flashes red for kRefuseFlashSec after a refused confirm, so
        // a green fret on a locked row is never "nothing happened"
        constexpr ImVec4 kRefusedNote{ 0.95f, 0.40f, 0.35f, 1.0f };
        constexpr ImVec4 kNewTag{ 0.12f, 0.90f, 1.0f, 1.0f };
        constexpr double kRefuseFlashSec = 1.5;

        enum SongColumn : ImGuiID {
            kSongColumn   = 0,
            kArtistColumn = 1,
            kLengthColumn = 2,
            kRatingColumn = 3,
            kDiffColumn   = 4
        };

        // ONE definition of the column list. BeginTable's declared count
        // must equal the number of TableSetupColumn calls or ImGui pops
        // "TableSetupColumn(): called too many times!" over the songbook -
        // which is exactly what shipped when the Diff column was added and
        // the two hardcoded 4s were left behind (field 2026-07-26).
        // Deriving the count from this array makes that drift impossible
        // rather than merely documented.
        struct SongColumnDef {
            const char* label;
            float       width;
            ImGuiID     id;
            bool        sortable;  // false = kNoSort, display only
        };

        inline constexpr SongColumnDef kSongColumnDefs[] = {
            { "Song", 0.38f, kSongColumn, true },
            { "Artist", 0.25f, kArtistColumn, true },
            { "Length", 0.12f, kLengthColumn, true },
            // Availability display only - the sort surface stays the four
            // established columns, or a header click would feed song_sort
            // an id it does not map.
            { "Diff", 0.10f, kDiffColumn, false },
            { "Rating", 0.15f, kRatingColumn, true },
        };
        inline constexpr int kSongColumnCount =
            static_cast<int>(std::size(kSongColumnDefs));

        void SetupSongColumns(bool a_sortable) {
            using CF = FUCK::TableColumnFlags;
            for (const auto& def : kSongColumnDefs) {
                CF flags = CF::kNone;
                if (!def.sortable) {
                    flags = CF::kNoSort;
                } else if (a_sortable && def.id == kRatingColumn) {
                    flags = CF::kDefaultSort | CF::kPreferSortDescending;
                }
                FUCK::TableSetupColumn(def.label, flags, def.width, def.id);
            }
        }

        constexpr const char* kDifficultyNames[
            list_navigation::kDifficultyCount] = { "EASY", "MEDIUM", "HARD",
                                                   "EXPERT" };

        // The difficulty a pick of this song would actually play right now:
        // the chart's own availability run through the same fallback
        // LoadSong uses, at the same requested difficulty Play() sends. An
        // unmeasured mask (scan failure) assumes the request.
        int RowDifficulty(const SongInfo& a_song, int a_want) {
            const int r = bard::ResolveDifficulty(a_song.diffMask, a_want);
            return r >= 0 ? r : a_want;
        }

        // E M H X availability glyphs: missing difficulties dim to near
        // background (the user asked to SEE what a Bridge chart lacks),
        // the one a pick would play draws gold, other present ones plain.
        void DrawDiffCell(std::uint8_t a_mask, int a_resolved,
                          bool a_locked) {
            static constexpr const char* kLetters[4] = { "E", "M", "H",
                                                         "X" };
            constexpr ImVec4 kMissing{ 0.30f, 0.29f, 0.27f, 0.85f };
            constexpr ImVec4 kPresent{ 0.72f, 0.70f, 0.66f, 1.0f };
            if (a_mask == 0) {
                FUCK::TextDisabled("-");
                return;
            }
            for (int d = 0; d < 4; ++d) {
                if (d > 0) { FUCK::SameLine(0.0f, FUCK::Scale(3.0f)); }
                const bool has = (a_mask >> d) & 1;
                ImVec4     color = has ? kPresent : kMissing;
                if (has && d == a_resolved) {
                    color = a_locked ? kLockedRow : panel::kGold;
                }
                FUCK::TextColored(color, "%s", kLetters[d]);
            }
        }

        float ButtonWidth(const char* a_label) {
            const ImVec2 padding =
                FUCK::GetStyleVarVec(ImGuiStyleVar_FramePadding);
            return FUCK::CalcTextSize(a_label).x + padding.x * 2.0f;
        }

        void DrawRatingStars(int a_earned) {
            constexpr float kPi = 3.14159265358979323846f;
            const float s = FUCK::Scale(1.0f);
            const float lineH = FUCK::GetTextLineHeight();
            const float outer = std::min(6.5f * s, lineH * 0.42f);
            const float inner = outer * 0.46f;
            const float gap = 3.0f * s;
            const ImVec2 p = FUCK::GetCursorScreenPos();
            const ImVec4 emptyFill{ 0.08f, 0.08f, 0.075f, 0.96f };
            const ImVec4 emptyEdge{ 0.46f, 0.44f, 0.39f, 0.92f };
            const ImVec4 earnedEdge{ 0.72f, 0.58f, 0.28f, 1.0f };

            for (int star = 0; star < 5; ++star) {
                const ImVec2 center(
                    p.x + outer + star * (outer * 2.0f + gap),
                    p.y + lineH * 0.5f);
                ImVec2 points[10];
                for (int i = 0; i < 10; ++i) {
                    const float radius = (i % 2 == 0) ? outer : inner;
                    const float angle = -kPi * 0.5f + i * kPi / 5.0f;
                    points[i] = ImVec2(center.x + std::cos(angle) * radius,
                                      center.y + std::sin(angle) * radius);
                }
                const bool earned = star < std::clamp(a_earned, 0, 5);
                const ImVec4 fill = earned ? panel::kGold : emptyFill;
                const ImVec4 edge = earned ? earnedEdge : emptyEdge;
                for (int i = 0; i < 10; ++i) {
                    FUCK::DrawTriangleFilled(center, points[i],
                                             points[(i + 1) % 10], fill);
                    FUCK::DrawLine(points[i], points[(i + 1) % 10],
                                   edge, s);
                }
            }
            FUCK::Dummy(ImVec2(5.0f * outer * 2.0f + 4.0f * gap, lineH));
        }

        class BrowserWindow final : public FUCK::IWindow {
        public:
            const char* Id() const override { return "SongBrowserV3"; }
            const char* Title() const override {
                static const std::string t =
                    std::string(kDisplayName) + " - Songs";
                return t.c_str();
            }
            bool        IsOpen() const override {
                auto& bus = UiBus::GetSingleton();
                int   context = songeligibility::kContextFree;
                if (bus.TakeBrowserOpenRequest(context)) {
                    auto* self = const_cast<BrowserWindow*>(this);
                    self->Open(context);
                    // field diagnostic (2026-07-19 no-browser report): the
                    // last boundary before the window is host-drawn
                    spdlog::info(
                        "[browser] open request consumed (instrument "
                        "context={})",
                        context);
                }
                // ...and the pause menu's "practice this song" route, which
                // opens the window ALREADY IN the picker for a known song.
                // Consumed after the ordinary open request on purpose: that
                // one may have just opened the window this frame, and this
                // only changes which view it lands on.
                {
                    bard::SongEntry pickEntry;
                    int             pickDiff = 3, pickCtx = -1;
                    if (bus.TakePracticePickerRequest(pickEntry, pickDiff,
                                                      pickCtx)) {
                        auto* self = const_cast<BrowserWindow*>(this);
                        self->Open(pickCtx);
                        self->EnterPracticePickerFor(pickEntry, pickDiff,
                                                     pickCtx);
                        spdlog::info(
                            "[practice] picker opened directly for {}",
                            path_text::Utf8(pickEntry.folder));
                    }
                }
                // a session starting (or active) force-closes the browser;
                // so does a save load (OnPreLoadGame close request)
                if ((EngineFeed::GetSingleton().active.load() ||
                     bus.browserCloseRequest.exchange(false)) &&
                    _open.load()) {
                    const_cast<BrowserWindow*>(this)->Close();
                }
                // mirror for the InputHook's browse-time swallow (guitar
                // navigation must not steer the character); called every
                // frame, so close transitions propagate within one frame
                const bool open = _open.load();
                bus.browserOpen.store(open);
                // Mirrored here rather than in Draw() for the same reason
                // browserOpen is: Draw stops being called on an external
                // close, and a latched flag would strand the picker's hints
                // on screen with no picker behind them.
                bus.practicePickerOpen.store(open && _practiceView);
                return open;
            }
            void SetOpen(bool open) override {
                if (open) Open(songeligibility::kContextFree);
                else      Close();
            }
            FUCK::WindowFlags GetFlags() const override {
                using F = FUCK::WindowFlags;
                // Host geometry mechanics (FUCK-Man.cpp, FR OS-54 precedent):
                // kNoMove pins the pos to GetDefaultPos() and kNoResize pins
                // the size to GetDefaultSize() EVERY frame - saved state and
                // in-Draw SetWindow* (which lands on the host's ##Content
                // child) never apply. kCustomPosition would skip positioning
                // entirely (field-found: window stuck tiny at top-left).
                return static_cast<F>(
                    static_cast<unsigned>(F::kNoDecoration) |
                    static_cast<unsigned>(F::kNoBackground) |
                    static_cast<unsigned>(F::kNoMove) |
                    static_cast<unsigned>(F::kNoResize) |
                    static_cast<unsigned>(F::kHideHUD) |
                    static_cast<unsigned>(F::kCloseOnEsc) |
                    // hide behind native menus like the siblings (else Play
                    // is clickable over a paused menu)
                    static_cast<unsigned>(F::kCloseOnGameMenu));
            }
            ImVec2 GetDefaultSize() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                return ImVec2(std::clamp(d.x * 0.46f, 680.0f, 960.0f),
                              std::clamp(d.y * 0.58f, 500.0f, 700.0f));
            }
            ImVec2 GetDefaultPos() const override {
                const ImVec2 d = FUCK::GetDisplaySize();
                const ImVec2 s = GetDefaultSize();
                return ImVec2((d.x - s.x) * 0.5f, (d.y - s.y) * 0.5f);
            }

            void Draw() override {
                auto& lib = SongLibrary::GetSingleton();
                // FLICK's IsWindowAppearing never fires for host-managed
                // IWindows (field-found: scan + cursor never armed, empty
                // browser) - one-shot open work self-latches instead.
                // EnsureScan is an internal once-latch, free after the
                // first call. Interactive panels deliberately acquire the
                // FLICK cursor immediately; kHideHUD removes Skyrim's
                // reticle without hiding that pointer.
                lib.EnsureScan();
                if (!_drawHeld) {
                    _drawHeld = true;
                    RenderUi::AcquireCursor();
                    _mouseCursorHeld = true;
                    // stale nav intents from before this open (e.g. strums
                    // during the trigger) must not fire into the fresh list
                    UiBus::GetSingleton().DrainNav();
                    _navRepeat.Reset();
                    _enterAt = FUCK::GetTime();  // P6 content slam-in
                }
                // geometry is host-pinned via GetDefaultSize/Pos (see
                // GetFlags comment) - no in-Draw SetWindow* calls here
                //
                // The practice picker is a VIEW of this window, so it takes
                // over the body after the shared first-draw work (cursor,
                // nav drain) has already run. Returning here is what keeps
                // it out of the song list's layout entirely.
                if (_practiceView) {
                    DrawPracticeView();
                    return;
                }
                auto songs = lib.Snapshot();
                auto& ledger = StarLedger::GetSingleton();
                const int progressionContext =
                    songeligibility::ProgressionContext(_instrumentContext);
                const auto activeInst =
                    progressionContext >= 0
                      ? static_cast<stars::Instrument>(progressionContext)
                      : ledger.ActiveInstrument();
                std::vector<int> order;
                order.reserve(songs->size());
                for (std::size_t i = 0; i < songs->size(); ++i) {
                    if (songeligibility::IsEligible(
                            (*songs)[i].instrument, _instrumentContext)) {
                        order.push_back(static_cast<int>(i));
                    }
                }
                // ---- the unlock gate (spec 6.2 / 6.4) --------------------
                // The rank MUST be the number the bard-teaching pick uses,
                // or a lesson buys a chart the browser still dims, or the
                // browser offers one the confirm path then refuses. So it
                // comes from the one shared helper - read its comment in
                // SgtProgression.h before touching anything here.
                //
                // The sample is UiSampled and NOT LiveExpertise because this
                // is the render thread, which must not read SGT's globals;
                // the browser pumps a game-thread sample at 1Hz exactly the
                // way the settings page does.
                if (FUCK::GetTime() >= _nextSample) {
                    SgtProgression::PostUiSample();
                    _nextSample = FUCK::GetTime() + 1.0;
                }
                const int sampledExp = SgtProgression::UiSampled(activeInst);
                // Fail-open twice over, in the same direction UnlockLogic.h
                // already promises. Without SGT there is no expertise ladder
                // AND no bard lesson, so a gated chart would sit there
                // reading "Rank 4 required" with no route on earth to rank 4.
                // And a sample of -1 means the game thread has not answered
                // yet, which is true for the first frames of the first open:
                // a permissive frame is recoverable, a wrongly refused start
                // is not.
                const bool gateOn =
                    SgtProgression::Available() && sampledExp >= 0;
                const int  playerRank =
                    SgtProgression::EffectiveRank(activeInst, sampledExp);
                // Required rank per locked chart, 0 = playable. Indexed by
                // RAW song index, never by display row: the soft sort above
                // reorders rows, and Play() below takes the raw index too, so
                // both the dimming and the refusal read the same slot.
                std::vector<int> lockNeed(songs->size(), 0);
                for (std::size_t i = 0; i < songs->size(); ++i) {
                    const auto& s = (*songs)[i];
                    const int   req = s.requiredRank;
                    const bool open =
                        !gateOn ||
                        unlock::IsUnlocked(
                            req, playerRank,
                            ledger.Taught(
                                path_text::Utf8(
                                    s.entry.folder.filename())));
                    lockNeed[i] = open ? 0 : req;
                }
                // Stars are per difficulty (GH convention): every read
                // below asks for the record at the difficulty a pick of
                // that row would actually play, from the same requested
                // difficulty Play() sends - which is now the Songbook's
                // own selection, mirrored to the FLICK setting on change.
                const int wantDiff = std::clamp(_difficulty, 0, 3);
                std::vector<song_sort::Key> sortKeys(songs->size());
                for (std::size_t i = 0; i < songs->size(); ++i) {
                    const auto& song = (*songs)[i];
                    const auto key =
                        path_text::Utf8(song.entry.folder.filename());
                    auto& sortKey       = sortKeys[i];
                    sortKey.song        = song.name;
                    sortKey.artist      = song.artist;
                    sortKey.stableIndex = static_cast<int>(i);
                    sortKey.lengthMs    = song.lengthMs;
                    if (lockNeed[i] > 0) {
                        sortKey.ratingState =
                            song_sort::RatingState::kLocked;
                        sortKey.ratingValue = lockNeed[i];
                    } else {
                        const int best = ledger.Best(
                            key, activeInst, RowDifficulty(song, wantDiff));
                        sortKey.ratingState =
                            best > 0 && !ledger.IsNew(key)
                                ? song_sort::RatingState::kRated
                                : song_sort::RatingState::kUnplayed;
                        sortKey.ratingValue = best;
                    }
                }
                song_sort::Sort(order, sortKeys, _sortColumn,
                                _sortAscending);
                // The scan snapshot can change asynchronously. Never let a
                // row index from the prior filtered view name a new song.
                if (_selected < -1 ||
                    _selected >= static_cast<int>(order.size())) {
                    _selected = -1;
                }
                // Guitar/keyboard navigation via UiBus intents harvested by
                // the InputHook from the swallowed key events (field round
                // 4: FLICK's ImGui never sees swallowed/injected keys, so
                // ImGui polling was dead). Strum bar (Up/Down) moves the
                // selection, green fret / Enter (guitar Plus) plays, and red
                // fret / Esc closes. Difficulty is intentionally owned by
                // the FLICK Settings tool, not by this selection surface.
                auto& nav = UiBus::GetSingleton();
                if (nav.navClose.exchange(false)) {
                    ui_sound::Play(ui_sound::Event::kCancel);
                    Close();
                    return;
                }
                // Practice arming (orange fret). Only a flag here - the
                // picker itself is not entered until a song is confirmed,
                // so arming stays a cheap, reversible decision the player
                // can see reflected in the footer before committing.
                if (nav.navToggle.exchange(false)) {
                    _practiceArmed = !_practiceArmed;
                    ui_sound::Play(_practiceArmed
                                       ? ui_sound::Event::kConfirm
                                       : ui_sound::Event::kCancel);
                    spdlog::info("[practice] songbook arming -> {}",
                                 _practiceArmed ? "on" : "off");
                }
                // Difficulty step (yellow/blue fret, Left/Right). Mirrored
                // into the persisted setting so the FLICK Settings tab and
                // the next session agree with what was picked here - GH
                // remembers your difficulty too.
                if (const int diffStep = nav.navDiff.exchange(0);
                    diffStep != 0) {
                    const int before = _difficulty;
                    _difficulty = list_navigation::StepDifficulty(
                        _difficulty, diffStep);
                    if (_difficulty != before) {
                        auto& st = Settings::GetSingleton();
                        st.difficulty = _difficulty;
                        st.Save();
                        ui_sound::Play(ui_sound::Event::kFocus);
                        spdlog::info("[browser] difficulty -> {}",
                                     kDifficultyNames[_difficulty]);
                    }
                }
                const int navMove = _navRepeat.Step(
                    nav.navMove.exchange(0), nav.navHeldMove.load(),
                    FUCK::GetTime());
                // gated off while the results box is up: its green-fret /
                // Enter close must not double as an instant song start
                bool navPlay = false;
                if (!nav.resultsReady.load()) {
                    navPlay = nav.navConfirm.exchange(false);
                }
                const int rows = static_cast<int>(order.size());
                if (navMove != 0 && rows > 0) {
                    const int before = _selected;
                    _selected = list_navigation::WrapIndex(
                        _selected, navMove, rows);
                    if (_selected != before) {
                        ui_sound::Play(ui_sound::Event::kFocus);
                    }
                    _navScroll = true;
                    if (!_gutterLoggedAfterNav) {
                        _gutterLogAfterNav = true;
                    }
                }
                panel::DrawCurrent();
                panel::BeginBoundedContent("##songbook_content");
                // P6: content slam-in (spec) - a shrinking spacer slides
                // the songbook content up into place; the window itself
                // cannot move in-Draw (FLICK no-op)
                {
                    const float et = std::clamp(
                        static_cast<float>(
                            (FUCK::GetTime() - _enterAt) / 0.22),
                        0.0f, 1.0f);
                    const float ee = et * et * (3.0f - 2.0f * et);
                    if (ee < 1.0f) {
                        FUCK::Dummy(ImVec2(
                            1.0f,
                            (1.0f - ee) * 16.0f * FUCK::Scale(1.0f)));
                    }
                }
                panel::Header("SONGBOOK");
                panel::PushControls();
                const float uiScale = FUCK::Scale(1.0f);
                // Named after the PROGRESSION instrument, never the selection
                // context. The two differ for exactly one case and it is the
                // case that matters: ProgressionContext maps guitar onto
                // kLute, because the Doom Lute deliberately reuses SGT's lute
                // expertise and payout ladder. So the number beside this
                // label has always been the LUTE's rank, and printing
                // "GUITAR RANK" over it claimed a separate guitar ladder that
                // does not exist - a player could reasonably wonder why it
                // never moved independently. It is a lute; it says lute.
                const char* progressionDisplay[3] = {
                    "LUTE", "FLUTE", "DRUM"
                };
                const char* instrumentDisplay =
                    progressionDisplay[static_cast<int>(activeInst)];
                char rankLine[80];
                if (sampledExp >= 0) {
                    std::snprintf(
                        rankLine, sizeof(rankLine), "%s RANK  %s",
                        instrumentDisplay,
                        results::StandingName(playerRank));
                } else {
                    std::snprintf(
                        rankLine, sizeof(rankLine), "%s RANK  CHECKING...",
                        instrumentDisplay);
                }
                FUCK::PushStyleColor(ImGuiCol_Text, panel::kQuiet);
                FUCK::CenteredText(rankLine, false);
                FUCK::PopStyleColor();
                // Practice arming, stated on the surface rather than left as
                // hidden modal state: the orange fret changes what the green
                // fret does, and an unannounced mode change is how a player
                // ends up in a picker they did not ask for.
                // One line in BOTH states rather than two different strings:
                // the label stays put and only its value and colour move, so
                // the header does not reflow as the mode is toggled.
                FUCK::PushStyleColor(
                    ImGuiCol_Text,
                    _practiceArmed ? panel::kGold : panel::kQuiet);
                FUCK::CenteredText(_practiceArmed ? "Practice Mode: On"
                                                  : "Practice Mode: Off",
                                   false);
                FUCK::PopStyleColor();

                // Difficulty picker (GH picks difficulty at song select).
                // Centered by measuring the row first: the four labels are
                // fixed strings, so their combined width is known before
                // anything is drawn and no second pass is needed.
                {
                    const float gap = 10.0f * uiScale;
                    float rowW = 0.0f;
                    for (const char* name : kDifficultyNames) {
                        rowW += ButtonWidth(name) + gap;
                    }
                    rowW -= gap;
                    const float availW = FUCK::GetContentRegionAvail().x;
                    FUCK::Dummy(ImVec2(1.0f, 2.0f * uiScale));
                    if (availW > rowW) {
                        FUCK::SetCursorPosX(FUCK::GetCursorPos().x +
                                            (availW - rowW) * 0.5f);
                    }
                    for (int d = 0; d < 4; ++d) {
                        if (d > 0) { FUCK::SameLine(0.0f, gap); }
                        const bool picked = d == _difficulty;
                        // The selected one reads as the gold the Diff
                        // column already uses for "this is what plays".
                        FUCK::PushStyleColor(
                            ImGuiCol_Text,
                            picked ? panel::kGold : panel::kQuiet);
                        FUCK::PushID(d);
                        if (FUCK::Button(kDifficultyNames[d])) {
                            if (_difficulty != d) {
                                _difficulty = d;
                                auto& st = Settings::GetSingleton();
                                st.difficulty = d;
                                st.Save();
                                ui_sound::Play(ui_sound::Event::kConfirm);
                                spdlog::info("[browser] difficulty -> {}",
                                             kDifficultyNames[d]);
                            }
                        }
                        FUCK::PopID();
                        FUCK::PopStyleColor();
                    }
                    FUCK::Dummy(ImVec2(1.0f, 6.0f * uiScale));
                }

                if (lib.Scanning()) {
                    FUCK::Spinner("##scan", FUCK::Scale(8.0f),
                                  FUCK::Scale(2.0f),
                                  ImVec4(0.70f, 0.70f, 0.67f, 1.0f));
                    FUCK::SameLine();
                    FUCK::TextDisabled("Scanning song library...");
                }
                if (lib.BadCount() > 0) {
                    FUCK::TextDisabled("%d rejected", lib.BadCount());
                }
                FUCK::Separator();

                const bool canPlay =
                    _selected >= 0 &&
                    _selected < static_cast<int>(order.size());
                const int selNeed =
                    canPlay ? lockNeed[order[_selected]] : 0;
                // Body and footer are separate measured zones. The former
                // negative-height child consumed the footer and left Play
                // below the panel clip rectangle in the 20:2x field frame.
                // Practice sits on its OWN row above Play, so the footer
                // reserve has to grow by that row - this number and the
                // widgets drawn below are one measurement, and the panel
                // gets a second scrollbar the moment they disagree. That is
                // the exact failure a stacked control caused before.
                const float kPracticeRowH = 38.0f;
                // MEASURED, not guessed. The reserve used to be these
                // constants alone, and they only ever matched one theme: a
                // FLICK skin with roomier frame padding, or the extra "or
                // learn this song from a bard" line, pushes the real footer
                // past the reserve and the whole PANEL grows a second
                // scrollbar next to the list's own. Two field reports before,
                // and a third on 2026-07-27 - the numbers had drifted again.
                //
                // _footerMeasured is last frame's actual footer height. One
                // frame of lag is invisible, and unlike a constant it cannot
                // disagree with the widgets, because it IS the widgets. The
                // constants below are only the first-frame fallback.
                const float footerFallback =
                    (selNeed > 0 ? 82.0f : 58.0f) * uiScale +
                    kPracticeRowH * uiScale;
                const float footerH = _footerMeasured > 1.0f
                                          ? _footerMeasured + 4.0f * uiScale
                                          : footerFallback;
                using TF = FUCK::TableFlags;
                // FLICK's public API does not expose TableSetupScrollFreeze.
                // Keep the header in a separate, non-scrolling table and put
                // only the identically-weighted body in the child below.
                //
                // The body child must reserve its vertical scrollbar from
                // frame one. With ImGui's automatic scrollbar, arrow-nav's
                // first SetScrollHereY made the bar appear and narrowed only
                // the body table; the independent header kept its old width,
                // visibly shifting every column. AlwaysVerticalScrollbar
                // makes the body viewport stable before and after scrolling,
                // and subtracting the same active style width from the header
                // gives both tables the identical column work width.
                const float scrollbarW =
                    FUCK::GetStyleVar(ImGuiStyleVar_ScrollbarSize);
                const float fullHeaderW = FUCK::GetContentRegionAvail().x;
                const float headerW =
                    std::max(1.0f, fullHeaderW - scrollbarW);
                const ImVec2 headerLo = FUCK::GetCursorScreenPos();
                const bool headerOk = FUCK::BeginTable(
                    "song_headers", kSongColumnCount,
                    TF::kSortable | TF::kNoSavedSettings |
                        TF::kSizingStretchProp,
                    ImVec2(headerW, 0.0f));
                if (headerOk) {
                    SetupSongColumns(true);
                    FUCK::TableHeadersRow();
                    if (auto* specs = FUCK::GetTableSortSpecs();
                        specs && specs->SpecsCount > 0 && specs->Specs) {
                        const auto& spec = specs->Specs[0];
                        const auto nextColumn =
                            static_cast<song_sort::Column>(
                                std::clamp<int>(
                                    static_cast<int>(spec.ColumnUserID),
                                    static_cast<int>(
                                        song_sort::Column::kSong),
                                    static_cast<int>(
                                        song_sort::Column::kRating)));
                        const bool nextAscending =
                            spec.SortDirection !=
                            ImGuiSortDirection_Descending;
                        if (nextColumn != _sortColumn ||
                            nextAscending != _sortAscending) {
                            const int selectedRaw =
                                _selected >= 0 &&
                                        _selected <
                                            static_cast<int>(order.size())
                                    ? order[_selected]
                                    : -1;
                            _sortColumn    = nextColumn;
                            _sortAscending = nextAscending;
                            song_sort::Sort(order, sortKeys, _sortColumn,
                                            _sortAscending);
                            if (selectedRaw >= 0) {
                                const auto it = std::find(
                                    order.begin(), order.end(), selectedRaw);
                                _selected =
                                    it == order.end()
                                        ? -1
                                        : static_cast<int>(
                                              it - order.begin());
                            }
                            _clickArm = -1;
                            ui_sound::Play(ui_sound::Event::kFocus);
                        }
                    }
                    FUCK::EndTable();
                    // The fixed header is deliberately narrower than the
                    // scrolling body so its column boundaries align after
                    // the body's permanent scrollbar reservation. Cover
                    // that reserved gutter with the same header background
                    // instead of exposing the panel's black backing as a
                    // cheap-looking cap above the scrollbar.
                    const float headerBottom =
                        FUCK::GetCursorScreenPos().y;
                    FUCK::DrawRectFilled(
                        ImVec2(headerLo.x + headerW, headerLo.y),
                        ImVec2(headerLo.x + fullHeaderW, headerBottom),
                        FUCK::GetStyleColorVec4(ImGuiCol_TableHeaderBg));
                }
                const float listH = std::max(
                    1.0f, FUCK::GetContentRegionAvail().y - footerH);
                // FLICK supplies the child surface. The field frame proved
                // its host theme can be much lighter than our row palette,
                // so own this background explicitly instead of relying on
                // a theme color outside BardHero's control.
                FUCK::PushStyleColor(
                    ImGuiCol_ChildBg,
                    ImVec4(0.025f, 0.025f, 0.023f, 1.0f));
                FUCK::BeginChild(
                    "##songlist", ImVec2(0, listH), false,
                    static_cast<int>(
                        ImGuiWindowFlags_AlwaysVerticalScrollbar));
                const ImVec2 bodyLo = FUCK::GetCursorScreenPos();
                const ImVec2 bodyAvail = FUCK::GetContentRegionAvail();
                if (!_gutterLoggedInitial || _gutterLogAfterNav) {
                    const char* state =
                        _gutterLoggedInitial ? "after arrow navigation"
                                             : "initial";
                    spdlog::info(
                        "[browser] stable scrollbar gutter {}: "
                        "header={:.1f} body={:.1f} bar={:.1f}",
                        state, headerW, bodyAvail.x, scrollbarW);
                    _gutterLoggedInitial = true;
                    if (_gutterLogAfterNav) {
                        _gutterLoggedAfterNav = true;
                        _gutterLogAfterNav = false;
                    }
                }
                FUCK::DrawRectFilled(
                    bodyLo,
                    ImVec2(bodyLo.x + bodyAvail.x, bodyLo.y + bodyAvail.y),
                    ImVec4(0.025f, 0.025f, 0.023f, 1.0f));
                const bool tableOk = FUCK::BeginTable(
                    "song_rows", kSongColumnCount,
                    TF::kRowBg | TF::kBordersInnerH | TF::kSizingStretchProp);
                if (tableOk) {
                    SetupSongColumns(false);
                    for (std::size_t n = 0; n < order.size(); ++n) {
                        const auto& s    = (*songs)[order[n]];
                        const int   need = lockNeed[order[n]];
                        FUCK::TableNextRow();
                        const bool selected =
                            _selected == static_cast<int>(n);
                        const ImU32 rowColor = selected
                            ? IM_COL32(66, 66, 61, 255)
                            : (n % 2 == 0 ? IM_COL32(15, 15, 14, 250)
                                          : IM_COL32(22, 22, 20, 250));
                        FUCK::TableSetBgColor(
                            FUCK::TableBgTarget::kRowBg0, rowColor);
                        FUCK::TableNextColumn();
                        // dim the row, not the stars column: spec 6.4 leaves
                        // the star display alone
                        if (need > 0) {
                            FUCK::PushStyleColor(ImGuiCol_Text, kLockedRow);
                        }
                        const char* label = s.name.empty()
                                                ? "(unnamed)"
                                                : s.name.c_str();
                        FUCK::PushID(static_cast<int>(n));
                        // The explicit row background above is the persistent
                        // selection treatment. SpanAllColumns makes mouse
                        // hover cover the same full row, matching the FLICK
                        // demo table pattern. The label is title-only now, so
                        // the old appended lock copy cannot cross into Artist.
                        const int beforeSelection = _selected;
                        const bool rowClicked = FUCK::Selectable(
                                label, false,
                                ImGuiSelectableFlags_SpanAllColumns,
                                ImVec2(0, 0));
                        if (rowClicked) {
                            _selected = static_cast<int>(n);
                        }
                        if (_navScroll &&
                            _selected == static_cast<int>(n)) {
                            FUCK::SetScrollHereY(0.35f);
                            _navScroll = false;
                        }
                        // arm expires after 300ms so a stale arm can't fire
                        // instantly on a much later click
                        const bool doubleClicked =
                            FUCK::IsItemHovered(0) &&
                            FUCK::IsMouseClicked(0, false) &&
                            _clickArm == static_cast<int>(n) &&
                            FUCK::GetTime() - _clickArmTime < 0.30;
                        if (FUCK::IsItemClicked(0)) {
                            _clickArm     = static_cast<int>(n);
                            _clickArmTime = FUCK::GetTime();
                        }
                        FUCK::PopID();
                        FUCK::TableNextColumn();
                        FUCK::TextUnformatted(s.artist.c_str(), nullptr);
                        FUCK::TableNextColumn();
                        if (s.lengthMs > 0.0) {
                            const int sec =
                                static_cast<int>(s.lengthMs / 1000.0);
                            FUCK::Text("%d:%02d", sec / 60, sec % 60);
                        } else {
                            FUCK::TextDisabled("-");
                        }
                        if (need > 0) { FUCK::PopStyleColor(); }
                        FUCK::TableNextColumn();
                        DrawDiffCell(s.diffMask,
                                     bard::ResolveDifficulty(s.diffMask,
                                                             wantDiff),
                                     need > 0);
                        FUCK::TableNextColumn();
                        if (need > 0) {
                            FUCK::TextColored(kLockedRow, "Rank %d", need);
                        } else if (ledger.IsNew(
                                       path_text::Utf8(
                                           s.entry.folder.filename()))) {
                            FUCK::TextColored(kNewTag, "NEW");
                        } else {
                            const int best = ledger.Best(
                                path_text::Utf8(
                                    s.entry.folder.filename()),
                                activeInst, RowDifficulty(s, wantDiff));
                            DrawRatingStars(best);
                        }
                        if (doubleClicked) {
                            Play(*songs, static_cast<std::size_t>(order[n]),
                                 lockNeed, _instrumentContext);
                        } else if (rowClicked &&
                                   _selected != beforeSelection) {
                            ui_sound::Play(ui_sound::Event::kFocus);
                        }
                    }
                    FUCK::EndTable();
                } else {
                    // fallback: hosts where BeginTable declines still get a
                    // usable browser (plain selectable rows, no columns)
                    for (std::size_t n = 0; n < order.size(); ++n) {
                        const auto& s    = (*songs)[order[n]];
                        const int   need = lockNeed[order[n]];
                        char        title[380];
                        LabelFor(title, sizeof(title), s.name, need);
                        char row[440];
                        const bool isNew = need == 0 && ledger.IsNew(
                            path_text::Utf8(s.entry.folder.filename()));
                        std::snprintf(row, sizeof(row), "%s%s  -  %s", title,
                                      isNew ? "  [NEW]" : "",
                                      s.artist.c_str());
                        if (need > 0) {
                            FUCK::PushStyleColor(ImGuiCol_Text, kLockedRow);
                        }
                        FUCK::PushID(static_cast<int>(n));
                        const int beforeSelection = _selected;
                        if (FUCK::Selectable(
                                row, _selected == static_cast<int>(n),
                                0, ImVec2(0, 0))) {
                            _selected = static_cast<int>(n);
                            if (_selected != beforeSelection) {
                                ui_sound::Play(ui_sound::Event::kFocus);
                            }
                        }
                        if (need > 0) { FUCK::PopStyleColor(); }
                        if (_navScroll &&
                            _selected == static_cast<int>(n)) {
                            FUCK::SetScrollHereY(0.35f);
                            _navScroll = false;
                        }
                        FUCK::PopID();
                    }
                }
                if (order.empty() && !lib.Scanning()) {
                    char emptyLine[120];
                    const auto instrument =
                        songeligibility::InstrumentName(_instrumentContext);
                    if (!instrument.empty()) {
                        std::snprintf(
                            emptyLine, sizeof(emptyLine),
                            "No %.*s songs are available for this instrument.",
                            static_cast<int>(instrument.size()),
                            instrument.data());
                    } else {
                        std::snprintf(emptyLine, sizeof(emptyLine),
                                      "No songs are available.");
                    }
                    FUCK::Dummy(ImVec2(0.0f, 28.0f * uiScale));
                    // The "how do I get out" hint lives on the shared bottom
                    // bar now, which is up in this state too.
                    FUCK::CenteredText(emptyLine, false);

                    // The line a field report earned (2026-07-29): a charter
                    // put working charts in the songs root, the scan FOUND
                    // them, and this screen said only "no songs available" -
                    // which reads as a broken scan and sent them hunting
                    // through MO2 folders. An untagged chart is invisible
                    // from every instrument's songbook, so if any exist,
                    // SAY the scan saw them and why they are not here.
                    int untagged = 0;
                    for (const auto& s : *songs) {
                        if (!songeligibility::TaggedInstrument(s.instrument)
                                 .has_value()) {
                            ++untagged;
                        }
                    }
                    if (untagged > 0 &&
                        songeligibility::IsBoundContext(_instrumentContext)) {
                        char foundLine[160];
                        std::snprintf(
                            foundLine, sizeof(foundLine),
                            "The scan did find %d song%s - hidden here "
                            "because %s no instrument tag.",
                            untagged, untagged == 1 ? "" : "s",
                            untagged == 1 ? "it carries" : "they carry");
                        FUCK::Dummy(ImVec2(0.0f, 10.0f * uiScale));
                        FUCK::CenteredText(foundLine, false);
                        if (_instrumentContext == songeligibility::kGuitar) {
                            FUCK::CenteredText(
                                "Move them into a folder named guitar and "
                                "they count as guitar songs.",
                                false);
                        } else {
                            char tagLine[120];
                            std::snprintf(
                                tagLine, sizeof(tagLine),
                                "Add \"instrument = %.*s\" to each song.ini "
                                "to show them here.",
                                static_cast<int>(instrument.size()),
                                instrument.data());
                            FUCK::CenteredText(tagLine, false);
                        }
                    }

                    // An empty list is the ONE place a player is guaranteed
                    // to be looking when they need this, and BardHero ships
                    // no songs and downloads none - so without it the mod
                    // just looks broken. Bridge is named because it is the
                    // standard Clone Hero chart browser and it writes
                    // straight into the folder below with no conversion.
                    // InstrumentName returns exactly the folder names the
                    // scanner looks for, so the path can name the instrument
                    // the player is actually holding rather than assuming
                    // guitar - this state is reachable on every instrument.
                    // Kept to roughly 65 characters a line: CenteredText does
                    // NOT wrap, so anything wider than the panel is simply
                    // lost off the edges.
                    char pathLine[160];
                    if (!instrument.empty()) {
                        // No "then rescan": the user-songs folder is watched
                        // and picked up live, so telling a player to run a
                        // step that already happened just invites them to
                        // think it failed.
                        std::snprintf(
                            pathLine, sizeof(pathLine),
                            "Charts go in your Bard Hero Songs\\%.*s folder.",
                            static_cast<int>(instrument.size()),
                            instrument.data());
                    } else {
                        std::snprintf(
                            pathLine, sizeof(pathLine),
                            "Charts go in your Bard Hero Songs folder, under "
                            "the instrument's name.");
                    }

                    FUCK::Dummy(ImVec2(0.0f, 10.0f * uiScale));
                    FUCK::PushStyleColor(ImGuiCol_Text, panel::kQuiet);
                    FUCK::CenteredText(
                        "BardHero ships no songs of its own.", false);
                    FUCK::CenteredText(
                        "The Roadie addon installs a starter setlist, and",
                        false);
                    FUCK::CenteredText(
                        "Bridge downloads anything else. Both on the mod page.",
                        false);
                    FUCK::CenteredText(pathLine, false);
                    FUCK::PopStyleColor();
                }
                FUCK::EndChild();
                FUCK::PopStyleColor();

                // Everything from here to the end of the Play row is "the
                // footer", and its measured height feeds the next frame's
                // list reserve. Anything added below must sit inside this
                // span or the measurement stops describing reality.
                const float footerTop = FUCK::GetCursorPos().y;

                FUCK::Separator();
                if (selNeed > 0) {
                    const bool flash =
                        FUCK::GetTime() - _refusedAt < kRefuseFlashSec;
                    if (Settings::GetSingleton().bardTeachingUnlocks) {
                        FUCK::TextColored(
                            flash ? kRefusedNote : kLockedNote,
                            "Rank %d required, or learn this song from a bard",
                            selNeed);
                    } else {
                        FUCK::TextColored(flash ? kRefusedNote : kLockedNote,
                                          "Rank %d required", selNeed);
                    }
                }
                // Play is the ONLY action in this footer. A Delete button
                // lived here briefly (2026-07-26) and was removed the same
                // day: BardHero treats the song library as READ ONLY, so
                // removing a bad chart is a file-manager job, not ours.
                // See SongLibrary.h for the full reasoning - do not add a
                // destructive control back without revisiting that.
                //
                // The footer also has no vertical budget. Stacking a second
                // control under Play pushed it past the bounded content,
                // which both hid the button behind a scroll and gave the
                // panel a second scrollbar next to the list's own (two
                // field reports). Anything added here has to go sideways.
                // Practice sits on its OWN row ABOVE Play. Its height is
                // reserved in footerH above; the two must be changed
                // together or the list overruns its zone and the panel grows
                // a second scrollbar beside the list's own.
                //
                // Colour alone carries the armed state here, which is
                // exactly why the header line above spells it out in words:
                // a colour-only control needs a text anchor somewhere on the
                // surface, or it is unreadable to anyone who has not been
                // told what gold means.
                const float practiceW = ButtonWidth("Practice");
                FUCK::SetCursorPosX(
                    FUCK::GetCursorPos().x +
                    (FUCK::GetContentRegionAvail().x - practiceW) * 0.5f);
                if (_practiceArmed) {
                    FUCK::PushStyleColor(ImGuiCol_Button,
                                         ImVec4(0.42f, 0.33f, 0.13f, 0.96f));
                    FUCK::PushStyleColor(ImGuiCol_ButtonHovered,
                                         ImVec4(0.55f, 0.43f, 0.18f, 0.98f));
                    FUCK::PushStyleColor(ImGuiCol_ButtonActive,
                                         ImVec4(0.66f, 0.52f, 0.22f, 1.0f));
                    FUCK::PushStyleColor(ImGuiCol_Text, panel::kGold);
                }
                const bool practicePressed = FUCK::Button("Practice");
                if (_practiceArmed) { FUCK::PopStyleColor(4); }
                if (practicePressed) {
                    _practiceArmed = !_practiceArmed;
                    ui_sound::Play(_practiceArmed
                                       ? ui_sound::Event::kConfirm
                                       : ui_sound::Event::kCancel);
                    spdlog::info("[practice] songbook arming -> {} (mouse)",
                                 _practiceArmed ? "on" : "off");
                }
                FUCK::Dummy(ImVec2(1.0f, 6.0f * uiScale));
                // greying Play is the affordance for a mouse; the note below
                // and the log line in Play() are what a guitar player gets
                const float playW = ButtonWidth("PLAY");
                FUCK::SetCursorPosX(
                    FUCK::GetCursorPos().x +
                    (FUCK::GetContentRegionAvail().x - playW) * 0.5f);
                FUCK::BeginDisabled(!canPlay || selNeed > 0);
                const bool pressed = FUCK::Button("PLAY");
                FUCK::EndDisabled();
                // navPlay is NOT swallowed by BeginDisabled - the intent came
                // from the InputHook, not from ImGui - so it still reaches
                // Play(), which is where the refusal lives.
                if ((pressed || navPlay) && canPlay) {
                    Play(*songs, static_cast<std::size_t>(order[_selected]),
                         lockNeed, _instrumentContext);
                }
                // Close the footer measurement. Read BEFORE PopControls so
                // the style that sized these widgets is still the one in
                // effect - popping first would measure against different
                // padding than the widgets were drawn with.
                _footerMeasured = FUCK::GetCursorPos().y - footerTop;
                panel::PopControls();
                panel::EndBoundedContent();
            }

        private:
            // Last frame's measured footer height, in pixels, feeding this
            // frame's list reserve. 0 until the first frame has drawn, which
            // is what the constant fallback covers.
            float _footerMeasured = 0.0f;

            // "Fire   -   Rank 4 required". The reason is formatted into the
            // row's own label rather than into a fifth column: the trailing
            // text travels with the title through the table AND the
            // no-table fallback, and needs no layout change.
            static void LabelFor(char* a_buf, std::size_t a_cap,
                                 const std::string& a_name, int a_need) {
                const char* name =
                    a_name.empty() ? "(unnamed)" : a_name.c_str();
                if (a_need > 0) {
                    std::snprintf(a_buf, a_cap, "%s   -   Rank %d required",
                                  name, a_need);
                } else {
                    std::snprintf(a_buf, a_cap, "%s", name);
                }
            }

            // THE choke point. Every Songbook entry into a session - Play,
            // green fret and double-click - comes through here with the RAW
            // song index, so the gate cannot be walked around by a path that
            // forgot to check.
            void Play(const std::vector<SongInfo>& songs, std::size_t n,
                      const std::vector<int>& lockNeed,
                      int a_instrumentContext) {
                if (n >= songs.size()) {
                    spdlog::warn(
                        "[browser] refused stale song index {} (library "
                        "size={})",
                        n, songs.size());
                    ui_sound::Play(ui_sound::Event::kCancel);
                    return;
                }
                // Revalidate against the frozen context at the final choke
                // point. A selection made under an old/full view cannot
                // bypass the instrument filter.
                if (!songeligibility::CanStart(
                        songs[n].instrument, a_instrumentContext)) {
                    spdlog::warn(
                        "[browser] refused \"{}\" - instrument tag \"{}\" "
                        "is ineligible for context {}",
                        path_text::Utf8(
                            songs[n].entry.folder.filename()),
                        songs[n].instrument, a_instrumentContext);
                    ui_sound::Play(ui_sound::Event::kCancel);
                    return;
                }
                const int need = n < lockNeed.size() ? lockNeed[n] : 0;
                if (need > 0) {
                    const double now = FUCK::GetTime();
                    // key auto-repeat can re-fire the confirm intent every
                    // frame; the flash window doubles as the log throttle
                    const bool quiet = now - _refusedAt < kRefuseFlashSec;
                    _refusedAt       = now;
                    if (!quiet) {
                        spdlog::info(
                            "[unlock] browser refused \"{}\" - locked "
                            "(needs rank {})",
                            path_text::Utf8(
                                songs[n].entry.folder.filename()), need);
                    }
                    ui_sound::Play(ui_sound::Event::kCancel);
                    return;  // browser stays open, selection stays put
                }
                ui_sound::Play(ui_sound::Event::kConfirm);
                // Practice takes the SAME choke point rather than a parallel
                // one, so the stale-index, eligibility and rank-gate refusals
                // above cannot be walked around by picking practice instead.
                // It diverges only after every one of them has passed.
                if (_practiceArmed) {
                    // Nav intents are drained on the view's FIRST DRAW, not
                    // here: the green fret that got us here is still being
                    // dispatched and would otherwise confirm row 0 instantly.
                    EnterPracticePickerFor(songs[n].entry,
                                           std::clamp(_difficulty, 0, 3),
                                           a_instrumentContext,
                                           songs[n].name);
                    return;
                }
                // The Songbook's own selection, not a re-read of the
                // setting: they are kept in sync on change, but this is
                // the surface the player just looked at.
                UiBus::GetSingleton().PushStart(
                    songs[n].entry, std::clamp(_difficulty, 0, 3),
                    a_instrumentContext);
                Close();
            }
            // One list column. Only the rows inside the scroll window are
            // emitted, so the child never needs a scrollbar - which both
            // avoids the Songbook's double-scrollbar trap and removes any
            // dependence on FUCK::SetScrollHereY, a version-2 call that
            // silently no-ops on a v1 host.
            void DrawSectionList(const char* a_id, const char* a_title,
                                 float a_w, float a_h, int a_visible,
                                 int a_count, int& a_top, int a_selected,
                                 bool a_focused, bool a_isStart) {
                const float z = FUCK::Scale(1.0f);
                FUCK::PushStyleColor(ImGuiCol_ChildBg,
                                     ImVec4(0.025f, 0.025f, 0.023f, 1.0f));
                FUCK::BeginChild(
                    a_id, ImVec2(a_w, a_h), false,
                    static_cast<int>(ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse));
                // The wheel has to move OUR window index. The child carries
                // NoScrollWithMouse because only the visible rows are ever
                // emitted, so ImGui has no overflow to scroll and would
                // silently ignore the wheel.
                if (a_count > a_visible && FUCK::IsWindowHovered(0)) {
                    const float wheel = FUCK::GetMouseWheel();
                    if (wheel != 0.0f) {
                        a_top -= static_cast<int>(wheel);
                        const int maxTop = a_count - a_visible;
                        if (a_top < 0) { a_top = 0; }
                        if (a_top > maxTop) { a_top = maxTop; }
                    }
                }
                FUCK::TextColored(a_focused ? panel::kGold : panel::kQuiet,
                                  "%s", a_title);
                if (a_count <= 0) {
                    // No markers is NORMAL, not a failure: most charts carry
                    // none. Offer whole song as the only row rather than
                    // refusing to open.
                    FUCK::Selectable("Whole song", true, 0,
                                     ImVec2(0, practice_layout::kRowHeight * z));
                    FUCK::EndChild();
                    FUCK::PopStyleColor();
                    return;
                }
                const int last = std::min(a_count, a_top + a_visible);
                for (int i = a_top; i < last; ++i) {
                    const auto& sec = _pickSections[
                        static_cast<std::size_t>(i)];
                    const int   mins = static_cast<int>(sec.time) / 60;
                    const int   secs = static_cast<int>(sec.time) % 60;
                    char        label[192];
                    std::snprintf(label, sizeof(label), "%d:%02d  %s##%s%d",
                                  mins, secs, sec.name.c_str(), a_id, i);
                    if (FUCK::Selectable(
                            label, i == a_selected, 0,
                            ImVec2(0, practice_layout::kRowHeight * z))) {
                        if (a_isStart) {
                            _pick.startIdx = i;
                            _pick.focus    = practice_pick::Focus::kStart;
                        } else {
                            _pick.endIdx = i;
                            _pick.focus  = practice_pick::Focus::kEnd;
                        }
                        ui_sound::Play(ui_sound::Event::kFocus);
                    }
                }
                FUCK::EndChild();
                FUCK::PopStyleColor();
            }

            // The section picker, drawn as a VIEW of the Songbook. It shows
            // the range practice::ResolveRange RETURNS and never re-derives
            // it, which is how reversed picks, an empty section list, a last
            // section running to song end and the lead-in clamp all stay in
            // exactly one tested place.
            void DrawPracticeView() {
                auto& bus = UiBus::GetSingleton();
                if (_pickFirstDraw) {
                    _pickFirstDraw = false;
                    // IsWindowAppearing never fires for host-managed windows,
                    // and the green fret that opened this view is still being
                    // dispatched - without this drain it confirms row 0 on
                    // the very first frame.
                    bus.DrainNav();
                    _navRepeat.Reset();
                }
                if (!_pickReady) {
                    _pickReady = bus.ReadPracticeSections(
                        _pickSections, _pickSongEnd, _practiceGen);
                }
                const int count = static_cast<int>(_pickSections.size());

                if (bus.navClose.exchange(false)) {
                    // Back to the song list, NOT out of the Songbook: red
                    // fret is "back" by CH convention, and the player is
                    // still mid-choice.
                    ui_sound::Play(ui_sound::Event::kCancel);
                    LeavePracticeView();
                    return;
                }
                if (bus.navToggle.exchange(false)) {
                    // The key that armed practice also disarms it.
                    ui_sound::Play(ui_sound::Event::kCancel);
                    _practiceArmed = false;
                    LeavePracticeView();
                    return;
                }
                const int navMove = _navRepeat.Step(
                    bus.navMove.exchange(0), bus.navHeldMove.load(),
                    FUCK::GetTime());
                const int  navStep = bus.navDiff.exchange(0);
                const bool confirm = bus.navConfirm.exchange(false);
                const auto prev = _pick;
                _pick = practice_pick::Step(_pick, navMove, navStep, count);
                const bool selMoved = _pick.startIdx != prev.startIdx ||
                                      _pick.endIdx != prev.endIdx ||
                                      _pick.focus != prev.focus;
                if (selMoved) { ui_sound::Play(ui_sound::Event::kFocus); }
                // Strum on the Speed and Loop stops - the controller's
                // route to both. Applied against the POST-step focus, same
                // as the lists.
                if (navMove != 0
                    && _pick.focus == practice_pick::Focus::kSpeed) {
                    const double before = _practiceSpeed;
                    _practiceSpeed = practice_pick::StepSpeedPreset(
                        _practiceSpeed, navMove);
                    if (_practiceSpeed != before) {
                        ui_sound::Play(ui_sound::Event::kFocus);
                    }
                }
                if (navMove != 0
                    && _pick.focus == practice_pick::Focus::kLoop) {
                    _practiceLoop = !_practiceLoop;
                    ui_sound::Play(_practiceLoop
                                       ? ui_sound::Event::kConfirm
                                       : ui_sound::Event::kCancel);
                }
                std::vector<bard::ChartSection> secs;
                secs.reserve(_pickSections.size());
                for (const auto& s : _pickSections) {
                    bard::ChartSection cs;
                    cs.time = s.time;
                    cs.name = s.name;
                    secs.push_back(std::move(cs));
                }
                // -1/-1 is what ResolveRange reads as "whole song", and is
                // exactly what an unmarked chart must send to the session.
                const int  pickStart = count > 0 ? _pick.startIdx : -1;
                const int  pickEnd   = count > 0 ? _pick.endIdx : -1;
                const auto range     = bard::practice::ResolveRange(
                    secs, pickStart, pickEnd, _pickSongEnd);
                // Confirm is refused until the answer is in. Starting on an
                // unanswered request would send -1/-1 for a chart that does
                // have sections, silently practising the whole song.
                if (confirm && _pickReady) {
                    ui_sound::Play(ui_sound::Event::kConfirm);
                    bus.PushPracticeStart(_practiceEntry, _practiceDifficulty,
                                          _instrumentContext, pickStart,
                                          pickEnd, _practiceLoop,
                                          _practiceSpeed);
                    Close();
                    return;
                }

                panel::DrawCurrent(panel::kAccent, 0.90f);
                panel::BeginBoundedContent(
                    "##practice_content", 20.0f,
                    static_cast<int>(ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse));
                panel::Header("PRACTICE");
                panel::PushControls();
                const float z = FUCK::Scale(1.0f);
                FUCK::PushStyleColor(ImGuiCol_Text, panel::kGold);
                FUCK::CenteredText(_practiceSong.c_str(), false);
                FUCK::PopStyleColor();
                if (!_pickReady) {
                    FUCK::Dummy(ImVec2(1.0f, 10.0f * z));
                    FUCK::CenteredText("Reading chart...", false);
                    panel::PopControls();
                    panel::EndBoundedContent();
                    return;
                }
                const ImVec2 avail = FUCK::GetContentRegionAvail();
                const auto   cols =
                    practice_layout::SplitColumns(avail.x / z);
                const float listH = practice_layout::ListHeight(avail.y / z);
                // MEASURED, not assumed - see VisibleRows. Everything here is
                // divided back to logical units because listH is logical and
                // only gets scaled again when it is handed to BeginChild.
                const float titleH =
                    FUCK::GetTextLineHeightWithSpacing() / z;
                const float padY =
                    FUCK::GetStyleVarVec(ImGuiStyleVar_WindowPadding).y / z;
                const float gapY =
                    FUCK::GetStyleVarVec(ImGuiStyleVar_ItemSpacing).y / z;
                const int   visible = practice_layout::VisibleRows(
                    listH, titleH + padY * 2.0f,
                    practice_layout::kRowHeight + gapY);
                // Chase the selection ONLY when it actually moved. Running
                // this every frame re-centres the window on the selected row
                // and silently undoes any mouse-wheel scroll on the very
                // frame it happens - which reads as "the wheel does nothing"
                // (field 2026-07-26). Keyboard nav still pulls the view
                // along, because that is the case where the selection moves.
                if (selMoved) {
                    _pickTopStart = practice_layout::FirstVisibleRow(
                        _pick.startIdx, visible, count, _pickTopStart);
                    _pickTopEnd = practice_layout::FirstVisibleRow(
                        _pick.endIdx, visible, count, _pickTopEnd);
                }
                // ...but always keep the wheel's own scrolling in range, and
                // handle the section list being republished shorter.
                const int maxTop = count > visible ? count - visible : 0;
                _pickTopStart = std::clamp(_pickTopStart, 0, maxTop);
                _pickTopEnd   = std::clamp(_pickTopEnd, 0, maxTop);
                DrawSectionList("##pick_start", "From", cols.width * z,
                                listH * z, visible, count, _pickTopStart,
                                _pick.startIdx,
                                _pick.focus == practice_pick::Focus::kStart,
                                true);
                FUCK::SameLine(cols.rightX * z);
                DrawSectionList("##pick_end", "To", cols.width * z,
                                listH * z, visible, count, _pickTopEnd,
                                _pick.endIdx,
                                _pick.focus == practice_pick::Focus::kEnd,
                                false);
                {
                    // Range only - the key hints live on the shared bottom
                    // -of-screen bar now. Carries the speed as a PERCENT
                    // because the preset buttons are named rather than
                    // numbered, and the exact figure still has to be
                    // readable somewhere.
                    char foot[224];
                    std::snprintf(
                        foot, sizeof(foot),
                        "Range %d:%02d - %d:%02d  (%.1fs)    Speed %d%%",
                        static_cast<int>(range.startSec) / 60,
                        static_cast<int>(range.startSec) % 60,
                        static_cast<int>(range.endSec) / 60,
                        static_cast<int>(range.endSec) % 60,
                        range.endSec - range.startSec,
                        static_cast<int>(_practiceSpeed * 100.0 + 0.5));
                    FUCK::PushStyleColor(ImGuiCol_Text, panel::kQuiet);
                    FUCK::CenteredText(foot, false);
                    FUCK::PopStyleColor();
                }
                // Speed presets, Guitar Hero style: pick the tempo before
                // the run; the -/= keys still move in 5% steps once it is
                // live. The table lives in practice_pick so the strum
                // stepping and these buttons cannot drift apart.
                {
                    // Gold angle brackets flank the row while its focus
                    // stop is active; the row re-centres including them.
                    const bool speedFocused =
                        _pick.focus == practice_pick::Focus::kSpeed;
                    const float markW =
                        FUCK::CalcTextSize("> ").x;
                    float row = 0.0f;
                    for (const auto& p : practice_pick::kSpeedPresets) {
                        row += ButtonWidth(p.name) + 6.0f * z;
                    }
                    if (speedFocused) { row += 2.0f * markW; }
                    FUCK::SetCursorPosX(
                        FUCK::GetCursorPos().x +
                        (FUCK::GetContentRegionAvail().x - row) * 0.5f);
                    if (speedFocused) {
                        FUCK::TextColored(panel::kGold, "> ");
                        FUCK::SameLine(0.0f, 0.0f);
                    }
                    bool first = true;
                    for (const auto& p : practice_pick::kSpeedPresets) {
                        if (!first) { FUCK::SameLine(0.0f, 6.0f * z); }
                        first = false;
                        // Compared on the INTEGER percent, not the double:
                        // 0.7 is not exactly representable, so a float
                        // equality would leave some presets unhighlighted.
                        const bool active =
                            static_cast<int>(_practiceSpeed * 100.0 + 0.5) ==
                            p.pct;
                        if (active) {
                            FUCK::PushStyleColor(
                                ImGuiCol_Button,
                                ImVec4(0.42f, 0.33f, 0.13f, 0.96f));
                            FUCK::PushStyleColor(ImGuiCol_Text, panel::kGold);
                        }
                        if (FUCK::Button(p.name)) {
                            _practiceSpeed = p.pct / 100.0;
                            ui_sound::Play(ui_sound::Event::kFocus);
                        }
                        if (active) { FUCK::PopStyleColor(2); }
                    }
                    if (speedFocused) {
                        FUCK::SameLine(0.0f, 0.0f);
                        FUCK::TextColored(panel::kGold, " <");
                    }
                }
                FUCK::Dummy(ImVec2(1.0f, 6.0f * z));
                // Loop toggle and Play, side by side and centred. Both
                // mouse-clickable; the strum toggles the loop while its
                // focus stop is active, and the green fret still confirms,
                // so every control here now has a guitar route.
                const bool loopFocused =
                    _pick.focus == practice_pick::Focus::kLoop;
                const char* loopLabel =
                    _practiceLoop ? "Loop: On" : "Loop: Off";
                const float loopMarkW = FUCK::CalcTextSize("> ").x;
                const float loopW =
                    std::max(ButtonWidth("Loop: On"), ButtonWidth("Loop: Off"));
                const float playW = ButtonWidth("Play");
                const float gap   = 14.0f * z;
                float rowW = loopW + gap + playW;
                if (loopFocused) { rowW += 2.0f * loopMarkW; }
                FUCK::SetCursorPosX(
                    FUCK::GetCursorPos().x +
                    (FUCK::GetContentRegionAvail().x - rowW) * 0.5f);
                if (loopFocused) {
                    FUCK::TextColored(panel::kGold, "> ");
                    FUCK::SameLine(0.0f, 0.0f);
                }
                if (_practiceLoop) {
                    FUCK::PushStyleColor(ImGuiCol_Button,
                                         ImVec4(0.42f, 0.33f, 0.13f, 0.96f));
                    FUCK::PushStyleColor(ImGuiCol_Text, panel::kGold);
                }
                if (FUCK::Button(loopLabel)) {
                    _practiceLoop = !_practiceLoop;
                    ui_sound::Play(_practiceLoop ? ui_sound::Event::kConfirm
                                                 : ui_sound::Event::kCancel);
                }
                if (_practiceLoop) { FUCK::PopStyleColor(2); }
                if (loopFocused) {
                    FUCK::SameLine(0.0f, 0.0f);
                    FUCK::TextColored(panel::kGold, " <");
                }
                FUCK::SameLine(0.0f, gap);
                bool startNow = false;
                FUCK::BeginDisabled(!_pickReady);
                if (FUCK::Button("Play")) { startNow = true; }
                FUCK::EndDisabled();
                panel::PopControls();
                panel::EndBoundedContent();
                // Acted on AFTER the content child is closed. Doing it inside
                // would return early past EndBoundedContent and leave ImGui's
                // BeginChild/EndChild unbalanced for the rest of the frame.
                if (startNow && _pickReady) {
                    ui_sound::Play(ui_sound::Event::kConfirm);
                    bus.PushPracticeStart(_practiceEntry, _practiceDifficulty,
                                          _instrumentContext, pickStart,
                                          pickEnd, _practiceLoop,
                                          _practiceSpeed);
                    Close();
                }
            }

            // Enter the picker for a KNOWN song. Shared by the Songbook's
            // armed Play and by the pause menu's direct route, so the two
            // cannot drift apart - the direct route in particular must set
            // every field, or it inherits whatever the last browse left.
            void EnterPracticePickerFor(const bard::SongEntry& a_entry,
                                        int a_diff, int a_context,
                                        const std::string& a_displayName =
                                            std::string()) {
                _instrumentContext  = a_context;
                _practiceEntry      = a_entry;
                _practiceSong       = a_displayName.empty()
                    ? path_text::Utf8(a_entry.folder.filename())
                    : a_displayName;
                _practiceDifficulty = std::clamp(a_diff, 0, 3);
                // Arm it too: the player arrived here by asking for
                // practice, and leaving the flag off would make the picker's
                // own Back button drop them into a Songbook that says
                // practice is disabled.
                _practiceArmed = true;
                // The session thread owns the parse; sections do not exist
                // in SongEntry. Until it answers, the view draws its waiting
                // state rather than an empty list that looks like a chart
                // with no markers.
                _practiceGen =
                    UiBus::GetSingleton().RequestPracticeSections(a_entry);
                _pick          = practice_pick::State{};
                _pickTopStart  = 0;
                _pickTopEnd    = 0;
                _pickSections.clear();
                _pickSongEnd   = 0.0;
                _pickReady     = false;
                _practiceView  = true;
                _pickFirstDraw = true;
            }

            void Open(int a_instrumentContext) {
                _instrumentContext = a_instrumentContext;
                _selected          = -1;
                _clickArm          = -1;
                // Re-read the persisted difficulty at every open: the
                // FLICK Settings tab can have moved it since the last
                // browse, and that surface stays authoritative between
                // sessions.
                _difficulty        = std::clamp(
                    Settings::GetSingleton().difficulty, 0, 3);
                // A fresh browse always starts on the song list. Practice
                // arming is deliberately NOT persisted: silently re-arming
                // it a session later would send the player into a picker
                // they did not ask for.
                LeavePracticeView();
                _practiceArmed = false;
                _open.store(true);
            }
            // One place that unwinds the sub-view, so close, cancel and
            // reopen cannot each forget a different field.
            void LeavePracticeView() {
                _practiceView = false;
                _pickFirstDraw = false;
                _pickReady    = false;
                _pickSections.clear();
                _pickSongEnd  = 0.0;
                _practiceGen  = 0;
                _pickTopStart = 0;
                _pickTopEnd   = 0;
                _pick         = practice_pick::State{};
                UiBus::GetSingleton().ClearPracticeSections();
            }
            void Close() {
                _open.store(false);
                LeavePracticeView();
                _clickArm = -1;  // an arm must not survive close/reopen
                // a stale refusal flash must not greet the next open, and
                // the next open must re-sample the expertise immediately
                _refusedAt  = -1000.0;
                _nextSample = 0.0;
                _drawHeld = false;
                _instrumentContext = songeligibility::kContextFree;
                if (_mouseCursorHeld) {
                    RenderUi::ReleaseCursor();
                    _mouseCursorHeld = false;
                }
            }
            // mutable: IsOpen() is const but consumes the open request
            mutable std::atomic<bool> _open{ false };
            int    _instrumentContext = songeligibility::kContextFree;
            int    _difficulty = 3;  // re-seeded from Settings at Open
            int    _selected = -1, _clickArm = -1;
            double _clickArmTime = 0.0;
            bool   _drawHeld = false, _mouseCursorHeld = false;
            double _enterAt = 0.0;
            song_sort::Column _sortColumn = song_sort::kDefaultColumn;
            bool   _sortAscending = song_sort::kDefaultAscending;
            bool   _navScroll   = false;  // scroll to the key-moved selection
            list_navigation::HeldRepeat _navRepeat;
            // ---- practice picker sub-view (plan P4) ----------------------
            // A VIEW of this window, not a second FLICK window: no extra
            // cursor refcount, no second open-mirror flag, and no third
            // `capture` value for InputHook.cpp's binary overlay ternaries
            // to mis-handle. See render/PracticeLayout.h.
            bool _practiceArmed = false;  // orange fret toggle on the list
            bool _practiceView  = false;  // the picker is showing
            bool _pickFirstDraw = false;  // drain nav on the view's 1st draw
            unsigned        _practiceGen = 0;   // section-request stamp
            bard::SongEntry _practiceEntry;
            std::string     _practiceSong;
            int             _practiceDifficulty = 3;
            practice_pick::State _pick;
            int _pickTopStart = 0, _pickTopEnd = 0;
            std::vector<UiBus::PracticeSection> _pickSections;
            double _pickSongEnd = 0.0;
            bool   _pickReady   = false;
            // Looping ON is the default: it is what practice mode has always
            // done, and the mode exists for drilling a passage repeatedly.
            bool   _practiceLoop = true;
            // Speed chosen BEFORE the run (Guitar Hero convention). The -/=
            // keys still adjust it live once playing.
            double _practiceSpeed = 1.0;
            bool   _gutterLoggedInitial = false;
            bool   _gutterLogAfterNav = false;
            bool   _gutterLoggedAfterNav = false;
            double _nextSample  = 0.0;    // next SGT expertise UI sample
            double _refusedAt   = -1000.0;  // last refused locked confirm
        };
        BrowserWindow g_browser;
    }

    void RegisterBrowserWindow() { FUCK::RegisterWindow(&g_browser); }
}
