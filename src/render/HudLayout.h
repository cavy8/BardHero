#pragma once

// Pure side-HUD composition. Runtime supplies the real measured text heights;
// tests sweep the supported FLICK resolution scales. Anchoring Star Power to
// the right panel's bottom keeps it independent of the score/multiplier font
// flow that caused the READY/x1/STAR POWER collision in HudScoreV4.

#include <algorithm>

namespace SH::hud_layout {
    struct Score {
        float titleY;
        float valueY;
        float multiplierY;
    };

    struct Glory {
        float titleY;
        float meterY;
        float powerLabelY;
        float powerMeterY;
    };

    inline Score MakeScore(float height, float scale, float titleH,
                           float valueH, float multiplierH) {
        const float pad = 14.0f * scale;
        const float gap = 7.0f * scale;
        Score out;
        out.titleY      = pad;
        out.valueY      = out.titleY + titleH + gap;
        out.multiplierY = std::max(height - pad - multiplierH,
                                   out.valueY + valueH + gap);
        return out;
    }

    inline Glory MakeGlory(float height, float scale, float titleH,
                           float labelH) {
        const float pad = 14.0f * scale;
        Glory out;
        out.titleY      = pad;
        out.meterY      = out.titleY + titleH + 10.0f * scale;
        out.powerMeterY = height - pad - 11.0f * scale;
        out.powerLabelY = out.powerMeterY - labelH - 7.0f * scale;
        return out;
    }

    inline float PanelWidth(float scale) {
        return 220.0f * std::max(scale, 0.01f);
    }

    inline float PanelHeight(float scale) {
        return 170.0f * std::max(scale, 0.01f);
    }
}
