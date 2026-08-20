#pragma once

#include <SimpleIni.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace SH::theme {
    inline constexpr const char* kThemeIniPath =
        "Data/SKSE/Plugins/BardHero/theme.ini";

    struct ThemeData {
        ImVec4 panel{ 0.032f, 0.032f, 0.030f, 0.86f };
        ImVec4 border{ 0.48f, 0.48f, 0.45f, 0.94f };
        ImVec4 accent{ 0.46f, 0.46f, 0.43f, 0.82f };
        ImVec4 text{ 0.88f, 0.88f, 0.85f, 1.0f };
        ImVec4 muted{ 0.62f, 0.62f, 0.59f, 0.95f };
        ImVec4 highlight{ 0.88f, 0.70f, 0.32f, 1.0f };
        ImVec4 control{ 0.16f, 0.16f, 0.15f, 0.94f };
        float rounding     = 9.0f;
        float shadowOffset = 5.0f;
        bool  innerBorder  = true;

        std::string highwayBackground;
        ImVec4 highwayBackgroundTint{ 1, 1, 1, 1 };
        ImVec4 highwayGradient{ 0.05f, 0.05f, 0.09f, 1.0f };
        ImVec4 highwayBorderLine{ 0.55f, 0.75f, 0.95f, 0.75f };
        ImVec4 highwayStrikeline{ 1.00f, 1.00f, 1.00f, 1.0f };
        ImVec4 highwayMeasureLine{ 1.00f, 1.00f, 1.00f, 0.34f };

        // Three optional full-screen decorative layers, each scaled (never
        // stretched) to fit the screen with letterbox/pillarbox bars, shown
        // only while the highway itself is showing. Underlay sits below
        // even the highway background image; midlayer sits above that
        // image but below the highway's own gems/HUD/banners; overlay sits
        // above all of that highway content.
        std::string highwayUnderlay;
        ImVec4 highwayUnderlayTint{ 1, 1, 1, 1 };
        std::string highwayMidlayer;
        ImVec4 highwayMidlayerTint{ 1, 1, 1, 1 };
        std::string highwayOverlay;
        ImVec4 highwayOverlayTint{ 1, 1, 1, 1 };

        ImVec4 fret[5] = {
            { 0.22f, 0.80f, 0.28f, 1.0f },
            { 0.90f, 0.22f, 0.20f, 1.0f },
            { 0.95f, 0.83f, 0.18f, 1.0f },
            { 0.25f, 0.55f, 0.95f, 1.0f },
            { 0.95f, 0.55f, 0.15f, 1.0f },
        };
        ImVec4 openNote{ 0.72f, 0.40f, 0.95f, 1.0f };
        ImVec4 miss{ 0.45f, 0.45f, 0.45f, 0.85f };
        ImVec4 starPower{ 0.10f, 0.92f, 1.00f, 1.0f };
    };

    inline ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return { a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t,
                 a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t };
    }

    namespace detail {
        inline std::string_view Trim(std::string_view s) {
            while (!s.empty() && std::isspace(
                       static_cast<unsigned char>(s.front()))) {
                s.remove_prefix(1);
            }
            while (!s.empty() && std::isspace(
                       static_cast<unsigned char>(s.back()))) {
                s.remove_suffix(1);
            }
            return s;
        }

        inline int HexDigit(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        inline bool HexByte(std::string_view s, std::uint8_t& out) {
            if (s.size() != 2) return false;
            const int hi = HexDigit(s[0]), lo = HexDigit(s[1]);
            if (hi < 0 || lo < 0) return false;
            out = static_cast<std::uint8_t>((hi << 4) | lo);
            return true;
        }

        inline bool ParseColor(std::string_view s, ImVec4& out) {
            s = Trim(s);
            if (!s.empty() && s.front() == '#') s.remove_prefix(1);
            if (s.size() != 6 && s.size() != 8) return false;
            std::uint8_t r=0,g=0,b=0,a=255;
            if (!HexByte(s.substr(0,2),r) ||
                !HexByte(s.substr(2,2),g) ||
                !HexByte(s.substr(4,2),b)) {
                return false;
            }
            if (s.size() == 8 && !HexByte(s.substr(6,2),a)) return false;
            constexpr float k = 1.0f / 255.0f;
            out = { r*k, g*k, b*k, a*k };
            return true;
        }

        inline ImVec4 ReadColor(CSimpleIniA& ini, const char* section,
                                const char* key,
                                const ImVec4& fallback) {
            const char* raw = ini.GetValue(section, key, nullptr);
            if (!raw) return fallback;
            ImVec4 parsed;
            return ParseColor(raw, parsed) ? parsed : fallback;
        }

        inline std::string ColorText(const ImVec4& c) {
            const auto byte = [](float v) {
                return std::clamp(
                    static_cast<int>(v * 255.0f + 0.5f), 0, 255);
            };
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X",
                          byte(c.x), byte(c.y), byte(c.z), byte(c.w));
            return buf;
        }

        inline ThemeData Load() {
            ThemeData t;
            CSimpleIniA ini;
            ini.SetUnicode();
            if (ini.LoadFile(kThemeIniPath) < 0) return t;

            t.panel = ReadColor(ini,"Menu","Panel",t.panel);
            t.border = ReadColor(ini,"Menu","Border",t.border);
            t.accent = ReadColor(ini,"Menu","Accent",t.accent);
            t.text = ReadColor(ini,"Menu","Text",t.text);
            t.muted = ReadColor(ini,"Menu","Muted",t.muted);
            t.highlight = ReadColor(ini,"Menu","Highlight",t.highlight);
            t.control = ReadColor(ini,"Menu","Control",t.control);
            t.rounding = static_cast<float>(std::clamp(
                ini.GetDoubleValue("Menu","fRounding",t.rounding),
                0.0, 32.0));
            t.shadowOffset = static_cast<float>(std::clamp(
                ini.GetDoubleValue("Menu","fShadowOffset",t.shadowOffset),
                0.0, 24.0));
            t.innerBorder = ini.GetBoolValue(
                "Menu","bInnerBorder",t.innerBorder);

            if (const char* bg = ini.GetValue(
                    "Highway","sBackground",nullptr)) {
                t.highwayBackground = std::string(Trim(bg));
            }
            t.highwayBackgroundTint = ReadColor(
                ini,"Highway","BackgroundTint",t.highwayBackgroundTint);
            t.highwayGradient = ReadColor(
                ini,"Highway","Gradient",t.highwayGradient);
            t.highwayBorderLine = ReadColor(
                ini,"Highway","BorderLine",t.highwayBorderLine);
            t.highwayStrikeline = ReadColor(
                ini,"Highway","Strikeline",t.highwayStrikeline);
            t.highwayMeasureLine = ReadColor(
                ini,"Highway","MeasureLine",t.highwayMeasureLine);

            if (const char* ul = ini.GetValue(
                    "Highway","sUnderlay",nullptr)) {
                t.highwayUnderlay = std::string(Trim(ul));
            }
            t.highwayUnderlayTint = ReadColor(
                ini,"Highway","UnderlayTint",t.highwayUnderlayTint);
            if (const char* ml = ini.GetValue(
                    "Highway","sMidlayer",nullptr)) {
                t.highwayMidlayer = std::string(Trim(ml));
            }
            t.highwayMidlayerTint = ReadColor(
                ini,"Highway","MidlayerTint",t.highwayMidlayerTint);
            if (const char* ov = ini.GetValue(
                    "Highway","sOverlay",nullptr)) {
                t.highwayOverlay = std::string(Trim(ov));
            }
            t.highwayOverlayTint = ReadColor(
                ini,"Highway","OverlayTint",t.highwayOverlayTint);

            t.fret[0] = ReadColor(ini,"Gameplay","FretGreen",t.fret[0]);
            t.fret[1] = ReadColor(ini,"Gameplay","FretRed",t.fret[1]);
            t.fret[2] = ReadColor(ini,"Gameplay","FretYellow",t.fret[2]);
            t.fret[3] = ReadColor(ini,"Gameplay","FretBlue",t.fret[3]);
            t.fret[4] = ReadColor(ini,"Gameplay","FretOrange",t.fret[4]);
            t.openNote = ReadColor(
                ini,"Gameplay","OpenNote",t.openNote);
            t.miss = ReadColor(ini,"Gameplay","Miss",t.miss);
            t.starPower = ReadColor(
                ini,"Gameplay","StarPower",t.starPower);
            return t;
        }
    }

    inline ThemeData& Mutable() {
        static ThemeData t = detail::Load();
        return t;
    }

    inline const ThemeData& Get() { return Mutable(); }

    inline void ResetDefaults() { Mutable() = ThemeData{}; }

    inline void Reload() { Mutable() = detail::Load(); }

    inline bool Save() {
        const auto& t = Get();
        CSimpleIniA ini;
        ini.SetUnicode();
        // Preserve the shipped comments/order where possible. A missing file
        // is also fine: SimpleIni will write the UI-owned keys from scratch.
        ini.LoadFile(kThemeIniPath);

        const auto setColor = [&](const char* section, const char* key,
                                  const ImVec4& value) {
            const std::string text = detail::ColorText(value);
            ini.SetValue(section, key, text.c_str());
        };

        setColor("Menu", "Panel", t.panel);
        setColor("Menu", "Border", t.border);
        setColor("Menu", "Accent", t.accent);
        setColor("Menu", "Text", t.text);
        setColor("Menu", "Muted", t.muted);
        setColor("Menu", "Highlight", t.highlight);
        setColor("Menu", "Control", t.control);
        ini.SetDoubleValue("Menu", "fRounding", t.rounding);
        ini.SetDoubleValue("Menu", "fShadowOffset", t.shadowOffset);
        ini.SetBoolValue("Menu", "bInnerBorder", t.innerBorder);

        ini.SetValue("Highway", "sBackground",
                     t.highwayBackground.c_str());
        setColor("Highway", "BackgroundTint", t.highwayBackgroundTint);
        setColor("Highway", "Gradient", t.highwayGradient);
        setColor("Highway", "BorderLine", t.highwayBorderLine);
        setColor("Highway", "Strikeline", t.highwayStrikeline);
        setColor("Highway", "MeasureLine", t.highwayMeasureLine);

        ini.SetValue("Highway", "sUnderlay", t.highwayUnderlay.c_str());
        setColor("Highway", "UnderlayTint", t.highwayUnderlayTint);
        ini.SetValue("Highway", "sMidlayer", t.highwayMidlayer.c_str());
        setColor("Highway", "MidlayerTint", t.highwayMidlayerTint);
        ini.SetValue("Highway", "sOverlay", t.highwayOverlay.c_str());
        setColor("Highway", "OverlayTint", t.highwayOverlayTint);

        static constexpr const char* kFretKeys[5] = {
            "FretGreen", "FretRed", "FretYellow", "FretBlue",
            "FretOrange"
        };
        for (int i = 0; i < 5; ++i) {
            setColor("Gameplay", kFretKeys[i], t.fret[i]);
        }
        setColor("Gameplay", "OpenNote", t.openNote);
        setColor("Gameplay", "Miss", t.miss);
        setColor("Gameplay", "StarPower", t.starPower);

        const auto rc = ini.SaveFile(kThemeIniPath);
        if (rc < 0) {
            spdlog::warn("[theme] failed to save {}", kThemeIniPath);
            return false;
        }
        return true;
    }
}
