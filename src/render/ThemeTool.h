#pragma once

// FLICK-native editor for the one BardHero theme. This intentionally lives
// as a separate ITool instead of inflating SettingsTool.cpp: it owns image
// preview lifetime, live application, and theme.ini persistence while the
// ordinary settings page remains gameplay/configuration focused.

#include <SimpleIni.h>
#include "FUCK_API.h"

#include "render/PanelStyle.h"
#include "render/RenderUi.h"
#include "render/Theme.h"

#include <algorithm>
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
                _saveFailed = false;
            }

            void OnClose() override {
                SaveNow();
                ReleasePreviewImage();
            }

            void Draw() override {
                MaybeSave();

                FUCK::TextDisabled(
                    "Edits the single BardHero theme. Color and geometry "
                    "changes apply immediately; the highway image reloads "
                    "when its path edit is committed.");

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
                    _saveFailed = false;
                }
                FUCK::SameLine();
                if (FUCK::Button("Reset theme defaults")) {
                    theme::ResetDefaults();
                    SyncBackgroundBuffer();
                    RenderUi::ApplyTheme(true);
                    RefreshPreviewImage();
                    QueueSave();
                }
                if (_saveFailed) {
                    FUCK::TextColored(
                        ImVec4(0.95f, 0.35f, 0.30f, 1.0f),
                        "Could not save theme.ini; check the log/path permissions.");
                } else if (_dirty) {
                    FUCK::TextDisabled("Saving changes...");
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

            void MaybeSave() {
                if (_dirty && FUCK::GetTime() >= _saveAt) SaveNow();
            }

            void SaveNow() {
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
                if (!_previewImage) return;
                if (auto* i = FUCK::GetInterface()) i->ReleaseImage(_previewImage);
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

            void EnsurePreviewImage() {
                const auto& path = theme::Get().highwayBackground;
                if (path != _previewPath) RefreshPreviewImage();
            }

            void DrawMenuEditor() {
                auto& t = theme::Mutable();
                bool changed = false;

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

                FUCK::SeparatorText("Live preview");
                DrawMenuPreview();
            }

            void DrawGameplayEditor() {
                auto& t = theme::Mutable();
                bool changed = false;

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

                FUCK::SeparatorText("Live preview");
                DrawGameplayPreview();
            }

            void DrawHighwayEditor() {
                auto& t = theme::Mutable();

                FUCK::SeparatorText("Background image");
                const bool pathEdited = FUCK::InputText(
                    "Image path", _backgroundPath, sizeof(_backgroundPath));
                if (pathEdited) {
                    t.highwayBackground = _backgroundPath;
                    QueueSave();
                }
                // Avoid disk/image churn on every typed character. The live
                // path is committed as soon as the user leaves the field or
                // presses Enter; tint still updates continuously below.
                if (FUCK::IsItemDeactivatedAfterEdit()) {
                    RenderUi::ApplyTheme(true);
                    RefreshPreviewImage();
                }
                FUCK::TextDisabled(
                    "Clone Hero standard is 1:2 width:height, e.g. "
                    "512x1024 or 1024x2048.");

                if (EditColor("Background tint", t.highwayBackgroundTint)) {
                    ApplyLive();
                }

                if (FUCK::Button("Reload background image")) {
                    t.highwayBackground = _backgroundPath;
                    RenderUi::ApplyTheme(true);
                    RefreshPreviewImage();
                    QueueSave();
                }

                EnsurePreviewImage();
                if (t.highwayBackground.empty()) {
                    FUCK::TextDisabled("No custom highway image selected.");
                } else if (!_previewImage && _previewAttempted) {
                    FUCK::TextColored(
                        t.miss,
                        "Image could not be loaded. The current path is kept.");
                } else if (_previewImage) {
                    const bool aspectOk = _previewW > 0.0f && _previewH > 0.0f &&
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

                FUCK::SeparatorText("Live preview");
                DrawHighwayPreview();
            }

            void DrawMenuPreview() {
                const auto& t = theme::Get();
                const float s = FUCK::Scale(1.0f);
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const float width = std::max(
                    260.0f * s,
                    std::min(FUCK::GetContentRegionAvail().x, 560.0f * s));
                const float height = 150.0f * s;
                const ImVec2 hi(origin.x + width, origin.y + height);

                panel::Draw(origin, hi, t.accent);

                FUCK::SetCursorScreenPos(
                    ImVec2(origin.x + 18.0f * s, origin.y + 14.0f * s));
                FUCK::PushStyleColor(ImGuiCol_Text, t.text);
                FUCK::PushFont(FUCK::GetFont(FUCK::Font::kLarge));
                FUCK::Text("Bard Hero");
                FUCK::PopFont();
                FUCK::TextColored(t.muted,
                                  "Songbook / pause / results presentation");
                panel::PushControls();
                FUCK::Button("Preview button##themePreview");
                FUCK::SameLine();
                FUCK::Checkbox("Preview toggle##themePreview",
                               &_previewToggle, false, true);
                panel::PopControls();
                FUCK::TextColored(t.highlight,
                                  "Highlighted information and earned stars");
                FUCK::PopStyleColor();

                FUCK::SetCursorScreenPos(
                    ImVec2(origin.x, hi.y + 8.0f * s));
            }

            void DrawGameplayPreview() {
                const auto& t = theme::Get();
                const float s = FUCK::Scale(1.0f);
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const float width = std::max(
                    280.0f * s,
                    std::min(FUCK::GetContentRegionAvail().x, 560.0f * s));
                const float height = 175.0f * s;
                const ImVec2 hi(origin.x + width, origin.y + height);

                const ImVec4 bg = theme::Mix(t.panel, ImVec4(0,0,0,1), 0.30f);
                FUCK::DrawRectFilled(origin, hi, bg, t.rounding * s * 0.5f);
                FUCK::DrawRect(origin, hi, t.border, t.rounding * s * 0.5f,
                               s);

                auto* i = FUCK::GetInterface();
                if (i) {
                    const float y = origin.y + 73.0f * s;
                    const float left = origin.x + 48.0f * s;
                    const float right = hi.x - 48.0f * s;
                    const float gap = (right - left) / 4.0f;
                    const float radius = 14.0f * s;
                    for (int lane = 0; lane < 5; ++lane) {
                        const ImVec2 c(left + gap * lane, y);
                        i->DrawCircleFilled(c, radius, t.fret[lane], 24);
                        i->DrawCircle(c, radius, t.border, 24, 2.0f * s);
                    }
                    i->DrawLine(ImVec2(left, y + 35.0f * s),
                                ImVec2(right, y + 35.0f * s),
                                t.openNote, 8.0f * s);
                }

                FUCK::SetCursorScreenPos(
                    ImVec2(origin.x + 16.0f * s, origin.y + 10.0f * s));
                FUCK::TextColored(t.text, "Five-fret lane palette");
                FUCK::SetCursorScreenPos(
                    ImVec2(origin.x + 16.0f * s, origin.y + 124.0f * s));
                FUCK::TextColored(t.openNote, "Open note");
                FUCK::SameLine();
                FUCK::TextColored(t.miss, "Miss");
                FUCK::SameLine();
                FUCK::TextColored(t.starPower, "Star Power");

                FUCK::SetCursorScreenPos(
                    ImVec2(origin.x, hi.y + 8.0f * s));
            }

            void DrawHighwayPreview() {
                const auto& t = theme::Get();
                const float s = FUCK::Scale(1.0f);
                const ImVec2 origin = FUCK::GetCursorScreenPos();
                const float width = std::max(
                    300.0f * s,
                    std::min(FUCK::GetContentRegionAvail().x, 560.0f * s));
                const float height = 240.0f * s;
                const ImVec2 hi(origin.x + width, origin.y + height);

                FUCK::DrawRectFilled(
                    origin, hi,
                    theme::Mix(t.panel, ImVec4(0,0,0,1), 0.45f),
                    t.rounding * s * 0.5f);

                auto* i = FUCK::GetInterface();
                if (i) {
                    const float cx = origin.x + width * 0.5f;
                    const float topY = origin.y + 18.0f * s;
                    const float bottomY = hi.y - 44.0f * s;
                    const float topHalf = 54.0f * s;
                    const float bottomHalf = std::min(width * 0.43f,
                                                      205.0f * s);
                    const ImVec2 p0(cx - topHalf, topY);
                    const ImVec2 p1(cx + topHalf, topY);
                    const ImVec2 p2(cx + bottomHalf, bottomY);
                    const ImVec2 p3(cx - bottomHalf, bottomY);

                    if (_previewImage) {
                        i->DrawImageQuad(
                            _previewImage, p0, p1, p2, p3,
                            ImVec2(0,0), ImVec2(1,0), ImVec2(1,1),
                            ImVec2(0,1), t.highwayBackgroundTint);
                    } else {
                        i->DrawQuadFilled(
                            p0, p1, p2, p3,
                            theme::Mix(t.panel, t.control, 0.35f));
                    }
                    i->DrawLine(p0, p3, t.border, 2.0f * s);
                    i->DrawLine(p1, p2, t.border, 2.0f * s);

                    // A few perspective beat lines make the image read as a
                    // highway rather than a generic trapezoid.
                    for (int row = 1; row <= 4; ++row) {
                        const float f = row / 5.0f;
                        const float y = topY + (bottomY - topY) * f;
                        const float hw = topHalf +
                            (bottomHalf - topHalf) * f;
                        ImVec4 beat = t.text;
                        beat.w = 0.18f;
                        i->DrawLine(ImVec2(cx - hw, y), ImVec2(cx + hw, y),
                                    beat, s);
                    }

                    const float laneGap = (bottomHalf * 2.0f) / 5.0f;
                    const float fretY = bottomY;
                    for (int lane = 0; lane < 5; ++lane) {
                        const float x = cx - bottomHalf + laneGap *
                            (lane + 0.5f);
                        i->DrawCircleFilled(ImVec2(x, fretY), 11.0f * s,
                                            t.fret[lane], 20);
                    }
                }

                FUCK::SetCursorScreenPos(
                    ImVec2(origin.x, hi.y + 8.0f * s));
            }

            char _backgroundPath[512]{};
            bool _dirty = false;
            bool _saveFailed = false;
            bool _previewToggle = true;
            double _saveAt = 0.0;

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
