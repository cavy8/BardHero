#pragma once

#include <SimpleIni.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace SH::theme {
    inline constexpr const char* kThemeIniPath = "Data/SKSE/Plugins/BardHero/theme.ini";

    struct ThemeData {
        ImVec4 panel{ 0.032f, 0.032f, 0.030f, 0.86f };
        ImVec4 border{ 0.48f, 0.48f, 0.45f, 0.94f };
        ImVec4 accent{ 0.46f, 0.46f, 0.43f, 0.82f };
        ImVec4 text{ 0.88f, 0.88f, 0.85f, 1.0f };
        ImVec4 muted{ 0.62f, 0.62f, 0.59f, 0.95f };
        ImVec4 highlight{ 0.88f, 0.70f, 0.32f, 1.0f };
        ImVec4 control{ 0.16f, 0.16f, 0.15f, 0.94f };
        float rounding = 9.0f;
        float shadowOffset = 5.0f;
        bool innerBorder = true;

        std::string highwayBackground;
        ImVec4 highwayBackgroundTint{ 1, 1, 1, 1 };

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
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
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
            if (!HexByte(s.substr(0,2),r) || !HexByte(s.substr(2,2),g) || !HexByte(s.substr(4,2),b)) return false;
            if (s.size() == 8 && !HexByte(s.substr(6,2),a)) return false;
            constexpr float k = 1.0f / 255.0f;
            out = { r*k, g*k, b*k, a*k };
            return true;
        }
        inline ImVec4 ReadColor(CSimpleIniA& ini, const char* section, const char* key, const ImVec4& fallback) {
            const char* raw = ini.GetValue(section, key, nullptr);
            if (!raw) return fallback;
            ImVec4 parsed;
            return ParseColor(raw, parsed) ? parsed : fallback;
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
            t.rounding = static_cast<float>(std::clamp(ini.GetDoubleValue("Menu","fRounding",t.rounding),0.0,32.0));
            t.shadowOffset = static_cast<float>(std::clamp(ini.GetDoubleValue("Menu","fShadowOffset",t.shadowOffset),0.0,24.0));
            t.innerBorder = ini.GetBoolValue("Menu","bInnerBorder",t.innerBorder);

            if (const char* bg = ini.GetValue("Highway","sBackground",nullptr)) t.highwayBackground = Trim(bg);
            t.highwayBackgroundTint = ReadColor(ini,"Highway","BackgroundTint",t.highwayBackgroundTint);

            t.fret[0] = ReadColor(ini,"Gameplay","FretGreen",t.fret[0]);
            t.fret[1] = ReadColor(ini,"Gameplay","FretRed",t.fret[1]);
            t.fret[2] = ReadColor(ini,"Gameplay","FretYellow",t.fret[2]);
            t.fret[3] = ReadColor(ini,"Gameplay","FretBlue",t.fret[3]);
            t.fret[4] = ReadColor(ini,"Gameplay","FretOrange",t.fret[4]);
            t.openNote = ReadColor(ini,"Gameplay","OpenNote",t.openNote);
            t.miss = ReadColor(ini,"Gameplay","Miss",t.miss);
            t.starPower = ReadColor(ini,"Gameplay","StarPower",t.starPower);
            return t;
        }
    }

    inline const ThemeData& Get() {
        static const ThemeData t = detail::Load();
        return t;
    }
}
