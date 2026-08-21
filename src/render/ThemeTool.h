#pragma once

// FLICK tool for editing the BardHero theme. It owns live application,
// previews, and theme.ini persistence. Previews use the real BardHero windows.

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
        // Editable path state and its preview image for a decorative layer.
        struct LayerFieldState {
            char path[512]{};
            bool editingDirty = false;
            void* previewImage = nullptr;
            bool previewAttempted = false;
            std::string previewPath;
            float previewW = 0.0f;
            float previewH = 0.0f;

            void Sync(const std::string& value) {
                std::snprintf(path, sizeof(path), "%s", value.c_str());
            }

            void ReleasePreview() {
                if (previewImage) {
                    if (auto* i = FUCK::GetInterface()) {
                        i->ReleaseImage(previewImage);
                    }
                }
                previewImage = nullptr;
                previewPath.clear();
                previewAttempted = false;
                previewW = previewH = 0.0f;
            }

            void RefreshPreview(const std::string& value) {
                ReleasePreview();
                if (value.empty()) return;
                auto* i = FUCK::GetInterface();
                if (!i) return;
                previewAttempted = true;
                previewPath = value;
                previewImage = i->LoadImage(value.c_str(), false);
                if (previewImage) {
                    i->GetImageInfo(previewImage, &previewW, &previewH);
                }
            }
        };

        class ThemeTool final : public FUCK::ITool {
        public:
            const char* Name() const override { return "Bard Hero Theme"; }

            void OnOpen() override {
                SyncBackgroundBuffer();
                _underlay.Sync(theme::Get().highwayUnderlay);
                _midlayer.Sync(theme::Get().highwayMidlayer);
                _overlay.Sync(theme::Get().highwayOverlay);
                RefreshAllPreviews();
                _dirty = false;
                _backgroundEditingDirty = false;
                _underlay.editingDirty = false;
                _midlayer.editingDirty = false;
                _overlay.editingDirty = false;
                _saveFailed = false;
            }

            void OnClose() override {
                SaveNow();
                ReleasePreviewImage();
                _underlay.ReleasePreview();
                _midlayer.ReleasePreview();
                _overlay.ReleasePreview();
            }

            void Draw() override {
                MaybeSave();

                FUCK::TextDisabled(
                    "Edit the BardHero theme. Preview tabs show the real "
                    "menu or highway behind this panel. Color and geometry "
                    "changes apply immediately; image paths reload on commit.");

                if (FUCK::Button("Save")) {
                    SaveNow();
                }
                FUCK::SameLine();
                if (FUCK::Button("Reload from file")) {
                    theme::Reload();
                    SyncBackgroundBuffer();
                    _underlay.Sync(theme::Get().highwayUnderlay);
                    _midlayer.Sync(theme::Get().highwayMidlayer);
                    _overlay.Sync(theme::Get().highwayOverlay);
                    RenderUi::ApplyTheme(true);
                    RefreshAllPreviews();
                    _dirty = false;
                    _backgroundEditingDirty = false;
                    _underlay.editingDirty = false;
                    _midlayer.editingDirty = false;
                    _overlay.editingDirty = false;
                    _saveFailed = false;
                }
                FUCK::SameLine();
                if (FUCK::Button("Reset defaults")) {
                    theme::ResetDefaults();
                    SyncBackgroundBuffer();
                    _underlay.Sync(theme::Get().highwayUnderlay);
                    _midlayer.Sync(theme::Get().highwayMidlayer);
                    _overlay.Sync(theme::Get().highwayOverlay);
                    RenderUi::ApplyTheme(true);
                    RefreshAllPreviews();
                    _backgroundEditingDirty = false;
                    _underlay.editingDirty = false;
                    _midlayer.editingDirty = false;
                    _overlay.editingDirty = false;
                    QueueSave();
                }
                if (_saveFailed) {
                    FUCK::TextColored(
                        ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
                        "Could not save theme.ini. Check the path and "
                        "permissions.");
                } else if (_dirty || AnyLayerPathEditingDirty()) {
                    FUCK::TextDisabled("Unsaved changes.");
                } else {
                    FUCK::TextDisabled("Saved.");
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

            // Commit a decorative layer path and refresh its preview.
            void CommitLayerPath(LayerFieldState& field, std::string& target) {
                if (!field.editingDirty) return;
                target = field.path;
                field.editingDirty = false;
                RenderUi::ApplyTheme(true);
                field.RefreshPreview(target);
                QueueSave();
            }

            bool AnyLayerPathEditingDirty() const {
                return _backgroundEditingDirty || _underlay.editingDirty ||
                       _midlayer.editingDirty || _overlay.editingDirty;
            }

            void RefreshAllPreviews() {
                RefreshPreviewImage();
                _underlay.RefreshPreview(theme::Get().highwayUnderlay);
                _midlayer.RefreshPreview(theme::Get().highwayMidlayer);
                _overlay.RefreshPreview(theme::Get().highwayOverlay);
            }

            void MaybeSave() {
                if (_dirty && FUCK::GetTime() >= _saveAt) SaveNow();
            }

            void SaveNow() {
                // Save and close commit any active path edit.
                CommitBackgroundPath();
                CommitLayerPath(_underlay, theme::Mutable().highwayUnderlay);
                CommitLayerPath(_midlayer, theme::Mutable().highwayMidlayer);
                CommitLayerPath(_overlay, theme::Mutable().highwayOverlay);
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
                // Avoid reloading the image on every keystroke.
                if (FUCK::IsItemDeactivatedAfterEdit()) {
                    CommitBackgroundPath();
                }
                FUCK::TextDisabled(
                    "Use a 1:2 image, such as 512x1024. It scrolls with the "
                    "notes; the top and bottom should connect.");

                if (EditColor("Background tint", t.highwayBackgroundTint)) {
                    ApplyLive();
                }

                FUCK::SeparatorText("Surface colors");
                bool changed = false;
                changed |= EditColor("Gradient", t.highwayGradient);
                changed |= EditColor("Border lines", t.highwayBorderLine);
                changed |= EditColor("Strikeline", t.highwayStrikeline);
                changed |= EditColor("Measure lines", t.highwayMeasureLine);
                if (changed) {
                    ApplyLive();
                }
                FUCK::TextDisabled(
                    "Beat lines use the measure-line color at lower opacity.");

                if (FUCK::Button("Reload background")) {
                    if (_backgroundEditingDirty) {
                        CommitBackgroundPath();
                    } else {
                        RenderUi::ApplyTheme(true);
                        RefreshPreviewImage();
                    }
                }

                if (t.highwayBackground.empty()) {
                    FUCK::TextDisabled("No highway background selected.");
                } else if (!_previewImage && _previewAttempted) {
                    FUCK::TextColored(
                        t.miss,
                        "Could not load image. Keeping the current path.");
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

                DrawLayerField(
                    "Underlay image",
                    "Drawn below the highway background.",
                    "Underlay", _underlay, t.highwayUnderlay,
                    t.highwayUnderlayTint);
                DrawLayerField(
                    "Midlayer image",
                    "Drawn between the background and highway effects.",
                    "Midlayer", _midlayer, t.highwayMidlayer,
                    t.highwayMidlayerTint);
                DrawLayerField(
                    "Overlay image",
                    "Drawn above the highway.",
                    "Overlay", _overlay, t.highwayOverlay,
                    t.highwayOverlayTint);
            }

            // Draw one full-screen decorative layer editor.
            void DrawLayerField(const char* sectionTitle, const char* hint,
                                const char* idSuffix, LayerFieldState& field,
                                std::string& target, ImVec4& tint) {
                FUCK::SeparatorText(sectionTitle);
                char pathId[64];
                std::snprintf(pathId, sizeof(pathId), "Image path##%s",
                             idSuffix);
                if (FUCK::InputText(pathId, field.path,
                                    sizeof(field.path))) {
                    field.editingDirty = true;
                }
                if (FUCK::IsItemDeactivatedAfterEdit()) {
                    CommitLayerPath(field, target);
                }
                FUCK::TextDisabled("%s", hint);
                FUCK::TextDisabled(
                    "Fits the screen without stretching; any aspect ratio "
                    "works.");

                char tintId[32];
                std::snprintf(tintId, sizeof(tintId), "Tint##%s", idSuffix);
                if (EditColor(tintId, tint)) ApplyLive();

                char reloadId[48];
                std::snprintf(reloadId, sizeof(reloadId), "Reload##%s",
                             idSuffix);
                if (FUCK::Button(reloadId)) {
                    if (field.editingDirty) {
                        CommitLayerPath(field, target);
                    } else {
                        RenderUi::ApplyTheme(true);
                        field.RefreshPreview(target);
                    }
                }

                if (target.empty()) {
                    FUCK::TextDisabled("No image selected.");
                } else if (!field.previewImage && field.previewAttempted) {
                    FUCK::TextColored(
                        theme::Get().miss,
                        "Could not load image. Keeping the current path.");
                } else if (field.previewImage) {
                    FUCK::TextDisabled("Loaded %.0fx%.0f",
                                       field.previewW, field.previewH);
                }
            }

            // Publish the target every frame so the heartbeat can expire it.
            void DrawMenuPreviewControls() {
                static const char* const kItems[] = { "Nothing", "Songbook",
                                                      "Results" };
                FUCK::SeparatorText("Preview");
                FUCK::Combo("Preview##themeMenusPreview", &_menuPreview,
                            kItems, 3);
                switch (_menuPreview) {
                    case 1:
                        theme_preview::Keep(
                            theme_preview::Target::kSongbook);
                        FUCK::TextDisabled(
                            "The real Songbook behind this panel. Song rows "
                            "are disabled in preview mode.");
                        break;
                    case 2:
                        theme_preview::Keep(
                            theme_preview::Target::kResults);
                        FUCK::TextDisabled(
                            "The real results panel with a settled sample run. "
                            "Celebration sounds are disabled.");
                        break;
                    default:
                        FUCK::TextDisabled(
                            "These colors apply to the Songbook, results, pause, "
                            "and practice panels.");
                        break;
                }
                if (_menuPreview != 0 &&
                    EngineFeed::GetSingleton().active.load(
                        std::memory_order_acquire)) {
                    FUCK::TextColored(
                        theme::Get().highlight,
                        "Unavailable during a song. Use the Gameplay or "
                        "Highway tabs instead.");
                }
            }

            void DrawHighwayPreviewControls() {
                FUCK::SeparatorText("Preview");
                FUCK::Checkbox("Show highway preview##themeHighwayPreview",
                               &_highwayPreview);
                if (_highwayPreview) {
                    theme_preview::Keep(theme_preview::Target::kHighway);
                }
                if (EngineFeed::GetSingleton().active.load(
                        std::memory_order_acquire)) {
                    FUCK::TextDisabled(
                        "Editing the active song.");
                } else {
                    FUCK::TextDisabled(
                        "The real highway on a short demo phrase. Pause a "
                        "song to edit its chart instead.");
                }
            }

            char _backgroundPath[512]{};
            bool _dirty = false;
            bool _backgroundEditingDirty = false;
            bool _saveFailed = false;
            double _saveAt = 0.0;

            // Menu preview starts on Songbook; highway preview is enabled.
            int  _menuPreview    = 1;
            bool _highwayPreview = true;

            // Used for the Highway tab's size and aspect readout.
            void* _previewImage = nullptr;
            bool _previewAttempted = false;
            std::string _previewPath;
            float _previewW = 0.0f;
            float _previewH = 0.0f;

            // Decorative full-screen highway layers.
            LayerFieldState _underlay;
            LayerFieldState _midlayer;
            LayerFieldState _overlay;
        };

        inline ThemeTool g_themeTool;
    }

    inline void RegisterThemeTool() {
        FUCK::RegisterTool(&theme_tool::g_themeTool);
    }
}
