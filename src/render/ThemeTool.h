#pragma once

// FLICK-native editor for the one BardHero theme. This intentionally lives
// as a separate ITool instead of inflating SettingsTool.cpp: it owns image
// preview lifetime, live application, and theme.ini persistence while the
// ordinary settings page remains gameplay/configuration focused.
//
// The preview is the REAL menu. Each tab publishes a theme_preview::Target
// every frame and the shipped windows put themselves on screen behind this
// page (FLICK draws its menu over registered IWindows - see
// PauseMenuWindow.cpp, which routes to this very menu mid-song for exactly
// that reason). Mock swatches drawn in this panel were the earlier design
// and were wrong in the way every mock is wrong: they showed a panel this
// file had to keep in step with BrowserWindow/ResultsWindow/HighwayWindow
// by hand, at a size and against a backdrop the player never sees.

#include <SimpleIni.h>
#include "FUCK_API.h"

#include "game/EngineFeed.h"
#include "render/RenderUi.h"
#include "render/Theme.h"
#include "render/ThemePreview.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace SH {
    namespace theme_tool {
        class ThemeTool final : public FUCK::ITool {
        public:
            const char* Name() const override { return "Bard Hero Theme"; }

            void OnOpen() override {
                SyncBackgroundBuffer();
                RefreshPreviewImage();
                _dirty = false;
                _backgroundEditingDirty = false;
                _saveFailed = false;
            }

            void OnClose() override {
                SaveNow();
                ReleasePreviewImage();
            }

            void Draw() override {
                MaybeSave();

                FUCK::TextDisabled(
                    "Edits the single BardHero theme. Each tab puts the real "
                    "menu it themes on screen behind this page, so what you "
                    "see is the finished result. Color and geometry changes "
                    "apply immediately; the highway image reloads when its "
                    "path edit is committed.");

                if (FUCK::Button("Save now")) {
                    SaveNow();
                }
                FUCK::SameLine();
                if (FUCK::Button("Reload from file")) {
                    theme::Reload();
                    SyncBackgroundBuffer();
                    RenderUi::ApplyTheme(true);
                    RefreshPreviewImage();
                    _dirty = false;
                    _backgroundEditingDirty = false;
                    _saveFailed = false;
                }
                FUCK::SameLine();
                if (FUCK::Button("Reset theme defaults")) {
                    theme::ResetDefaults();
                    SyncBackgroundBuffer();
                    RenderUi::ApplyTheme(true);
                    RefreshPreviewImage();
                    _backgroundEditingDirty = false;
                    QueueSave();
                }
                if (_saveFailed) {
                    FUCK::TextColored(
                        ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
                        "Could not save theme.ini; check the log/path permissions.");
                } else if (_dirty || _backgroundEditingDirty) {
                    FUCK::TextDisabled("Unsaved theme changes.");
                } else {
                    FUCK::TextDisabled("Theme saved.");
                }

                if (FUCK::BeginTabBar("##BardHeroThemeTabs")) {
                    if (FUCK::BeginTabItem("Menus")) {
                        DrawMenuEditor();
                        FUCK::EndTabItem();
                    }
                    if (FUCK::BeginTabItem("Gameplay")) {
                        DrawGameplayEditor();
                        FUCK::EndTabItem();
                    }
                    if (FUCK::BeginTabItem("Highway")) {
                        DrawHighwayEditor();
                        FUCK::EndTabItem();
                    }
                    FUCK::EndTabBar();
                }
            }

        private:
            static bool EditColor(const char* label, ImVec4& color) {
                return FUCK::ColorEdit4(label, &color.x);
            }

            void QueueSave() {
                _dirty = true;
                _saveAt = FUCK::GetTime() + 0.35;
            }

            void ApplyLive(bool reloadHighwayImage = false) {
                RenderUi::ApplyTheme(reloadHighwayImage);
                QueueSave();
            }

            void CommitBackgroundPath() {
                if (!_backgroundEditingDirty) return;
                theme::Mutable().highwayBackground = _backgroundPath;
                _backgroundEditingDirty = false;
                RenderUi::ApplyTheme(true);
                RefreshPreviewImage();
                QueueSave();
            }

            void MaybeSave() {
                if (_dirty && FUCK::GetTime() >= _saveAt) SaveNow();
            }

            void SaveNow() {
                // Clicking Save or closing FLICK is also a field commit, so a
                // path typed without tabbing away is never silently lost.
                CommitBackgroundPath();
                if (!_dirty) return;
                _saveFailed = !theme::Save();
                _dirty = _saveFailed;
                if (_saveFailed) _saveAt = FUCK::GetTime() + 2.0;
            }

            void SyncBackgroundBuffer() {
                std::snprintf(_backgroundPath, sizeof(_backgroundPath), "%s",
                              theme::Get().highwayBackground.c_str());
            }

            void ReleasePreviewImage() {
                if (_previewImage) {
                    if (auto* i = FUCK::GetInterface()) {
                        i->ReleaseImage(_previewImage);
                    }
                }
                _previewImage = nullptr;
                _previewPath.clear();
                _previewAttempted = false;
                _previewW = _previewH = 0.0f;
            }

            void RefreshPreviewImage() {
                ReleasePreviewImage();
                const auto& path = theme::Get().highwayBackground;
                if (path.empty()) return;
                auto* i = FUCK::GetInterface();
                if (!i) return;
                _previewAttempted = true;
                _previewPath = path;
                _previewImage = i->LoadImage(path.c_str(), false);
                if (_previewImage) {
                    i->GetImageInfo(_previewImage, &_previewW, &_previewH);
                }
            }

            void DrawMenuEditor() {
                auto& t = theme::Mutable();
                bool changed = false;

                DrawMenuPreviewControls();

                FUCK::SeparatorText("Palette");
                changed |= EditColor("Panel", t.panel);
                changed |= EditColor("Border", t.border);
                changed |= EditColor("Accent", t.accent);
                changed |= EditColor("Text", t.text);
                changed |= EditColor("Muted text", t.muted);
                changed |= EditColor("Highlight", t.highlight);
                changed |= EditColor("Controls", t.control);

                FUCK::SeparatorText("Shape");
                changed |= FUCK::SliderFloat(
                    "Rounding", &t.rounding, 0.0f, 32.0f, "%.0f px");
                changed |= FUCK::SliderFloat(
                    "Shadow offset", &t.shadowOffset, 0.0f, 24.0f,
                    "%.0f px");
                if (FUCK::Checkbox("Inner border", &t.innerBorder)) {
                    changed = true;
                }

                if (changed) ApplyLive();
            }

            void DrawGameplayEditor() {
                auto& t = theme::Mutable();
                bool changed = false;

                DrawHighwayPreviewControls();

                FUCK::SeparatorText("Fret lanes");
                changed |= EditColor("Green fret", t.fret[0]);
                changed |= EditColor("Red fret", t.fret[1]);
                changed |= EditColor("Yellow fret", t.fret[2]);
                changed |= EditColor("Blue fret", t.fret[3]);
                changed |= EditColor("Orange fret", t.fret[4]);

                FUCK::SeparatorText("Special states");
                changed |= EditColor("Open note", t.openNote);
                changed |= EditColor("Missed note", t.miss);
                changed |= EditColor("Star Power", t.starPower);

                if (changed) ApplyLive();
            }

            void DrawHighwayEditor() {
                auto& t = theme::Mutable();

                DrawHighwayPreviewControls();

                FUCK::SeparatorText("Background image");
                if (FUCK::InputText(
                        "Image path", _backgroundPath,
                        sizeof(_backgroundPath))) {
                    _backgroundEditingDirty = true;
                }
                // Loading half-typed filenames through FLICK every keypress
                // is both noisy and expensive. Commit as soon as the edit is
                // finished; every color/geometry control remains truly live.
                if (FUCK::IsItemDeactivatedAfterEdit()) {
                    CommitBackgroundPath();
                }
                FUCK::TextDisabled(
                    "Clone Hero standard is 1:2 width:height, e.g. "
                    "512x1024 or 1024x2048.");

                if (EditColor("Background tint", t.highwayBackgroundTint)) {
                    ApplyLive();
                }

                if (FUCK::Button("Reload background image")) {
                    if (_backgroundEditingDirty) {
                        CommitBackgroundPath();
                    } else {
                        RenderUi::ApplyTheme(true);
                        RefreshPreviewImage();
                    }
                }

                if (t.highwayBackground.empty()) {
                    FUCK::TextDisabled("No custom highway image selected.");
                } else if (!_previewImage && _previewAttempted) {
                    FUCK::TextColored(
                        t.miss,
                        "Image could not be loaded. The current path is kept.");
                } else if (_previewImage) {
                    const bool aspectOk =
                        _previewW > 0.0f && _previewH > 0.0f &&
                        std::abs(_previewW / _previewH - 0.5f) <= 0.01f;
                    if (aspectOk) {
                        FUCK::TextDisabled("Loaded %.0fx%.0f (1:2)",
                                           _previewW, _previewH);
                    } else {
                        FUCK::TextColored(
                            t.highlight,
                            "Loaded %.0fx%.0f; expected a 1:2 image.",
                            _previewW, _previewH);
                    }
                }
            }

            // Both of these publish EVERY frame the tab is drawn rather than
            // on change. That is what makes the heartbeat in ThemePreview.h
            // work: switching tabs, switching tools or closing the menu all
            // stop the publishing, and the previewed window notices by
            // itself. Nothing here has to remember to clean up.
            void DrawMenuPreviewControls() {
                static const char* const kItems[] = { "Nothing", "Songbook",
                                                      "Results" };
                FUCK::SeparatorText("On screen");
                FUCK::Combo("Preview##themeMenusPreview", &_menuPreview,
                            kItems, 3);
                switch (_menuPreview) {
                    case 1:
                        theme_preview::Keep(
                            theme_preview::Target::kSongbook);
                        FUCK::TextDisabled(
                            "The real Songbook, behind this page. Song rows "
                            "do nothing while it is a preview.");
                        break;
                    case 2:
                        theme_preview::Keep(
                            theme_preview::Target::kResults);
                        FUCK::TextDisabled(
                            "The real results panel, on a sample run. Its "
                            "celebration sounds are held while previewing.");
                        break;
                    default:
                        FUCK::TextDisabled(
                            "These colors dress the Songbook, results, pause "
                            "and practice panels.");
                        break;
                }
                if (_menuPreview != 0 &&
                    EngineFeed::GetSingleton().active.load(
                        std::memory_order_acquire)) {
                    // Both windows force-close on an active feed by design
                    // (a starting song must not leave either on screen), so
                    // say so rather than letting the preview look broken.
                    FUCK::TextColored(
                        theme::Get().highlight,
                        "Not shown during a song. Quit to the world, or use "
                        "the Gameplay/Highway tabs instead.");
                }
            }

            void DrawHighwayPreviewControls() {
                FUCK::SeparatorText("On screen");
                FUCK::Checkbox("Preview the highway##themeHighwayPreview",
                               &_highwayPreview);
                if (_highwayPreview) {
                    theme_preview::Keep(theme_preview::Target::kHighway);
                }
                if (EngineFeed::GetSingleton().active.load(
                        std::memory_order_acquire)) {
                    FUCK::TextDisabled(
                        "Your own song is on screen; edits land on it "
                        "directly.");
                } else {
                    FUCK::TextDisabled(
                        "The real highway on a short demo phrase, at its "
                        "true size. Pause a song and reopen this page to "
                        "tune against your own chart instead.");
                }
            }

            char _backgroundPath[512]{};
            bool _dirty = false;
            bool _backgroundEditingDirty = false;
            bool _saveFailed = false;
            double _saveAt = 0.0;

            // Preview targets, per tab. Both default to showing something:
            // the point of the page is to watch the menu change, and a
            // preview you have to switch on first is one most people will
            // never find.
            int  _menuPreview    = 1;  // index into kItems: Songbook
            bool _highwayPreview = true;

            // Kept for the Highway tab's size/aspect readout only - the
            // image itself is now shown by the real background layer.
            void* _previewImage = nullptr;
            bool _previewAttempted = false;
            std::string _previewPath;
            float _previewW = 0.0f;
            float _previewH = 0.0f;
        };

        inline ThemeTool g_themeTool;
    }

    inline void RegisterThemeTool() {
        FUCK::RegisterTool(&theme_tool::g_themeTool);
    }
}
