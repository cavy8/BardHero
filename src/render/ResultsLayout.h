#pragma once

// Pure screen-space layout for the lower Results section. Keeping these
// coordinates out of the FLICK Draw call lets the headless suite enforce the
// one invariant the 20:4x field frame broke: content must end above footerY.
namespace SH::results_layout {
    struct Upper {
        float scoreTitleY;
        float scoreValueY;
        float ratingY;
        float badgeY;
        float statsTitleY;
        float statsValueY;
        float splitY;
    };

    constexpr Upper MakeUpper(float topY, float scale) {
        return {
            topY + 86.0f * scale,
            topY + 115.0f * scale,
            topY + 166.0f * scale,
            topY + 179.0f * scale,
            topY + 209.0f * scale,
            topY + 242.0f * scale,
            topY + 278.0f * scale,
        };
    }

    struct Lower {
        float rewardSummaryY;
        float rewardDetailY;
        float progressTitleY;
        float progressSummaryY;
        float progressBarY;
        float progressBarH;
    };

    constexpr Lower MakeLower(float splitY, float footerY, float scale) {
        const float safeBottom = footerY - 12.0f * scale;
        const float barH = 8.0f * scale;
        // The lowest the meter may sit without clearing the footer.
        const float barFloor = safeBottom - barH;
        // The field frame is only about 100 logical pixels tall here after
        // FLICK scaling. Three reward rows cannot fit once the value uses the
        // large font, so V7 deliberately uses one "REWARD  1 GOLD" summary
        // plus one reason line. The progress side remains title, summary,
        // meter. Both fit the captured height without footer-derived rows
        // being pulled upward into siblings.
        //
        // ⚠ EVERY ROW HANGS FROM splitY, THE METER INCLUDED. It used to be
        // pinned to the footer instead (barY = safeBottom - barH), which is
        // right only while the frame is exactly as short as the comment above
        // assumes. The reward column is top-anchored, so on a taller frame
        // the two columns pulled apart and left a hole down the middle of the
        // panel - reported from the field 2026-07-27 off a full-combo results
        // screen. Surplus height now falls BELOW the block as an even bottom
        // margin, which is what the reward column always did.
        const float progressSummaryNatural = splitY + 42.0f * scale;
        const float barNatural = progressSummaryNatural + 30.0f * scale;
        const float barY = barNatural < barFloor ? barNatural : barFloor;
        const float progressSummaryLimit = barY - 28.0f * scale;
        const float progressSummary =
            progressSummaryNatural < progressSummaryLimit
                ? progressSummaryNatural
                : progressSummaryLimit;
        return {
            splitY + 8.0f * scale,
            splitY + 48.0f * scale,
            splitY + 8.0f * scale,
            progressSummary,
            barY,
            barH,
        };
    }

    constexpr float ContentBottom(const Lower& a_layout, float scale) {
        const float rewardBottom = a_layout.rewardDetailY + 20.0f * scale;
        const float progressTextBottom =
            a_layout.progressSummaryY + 20.0f * scale;
        const float progressBarBottom =
            a_layout.progressBarY + a_layout.progressBarH;
        float bottom = rewardBottom > progressTextBottom
                           ? rewardBottom
                           : progressTextBottom;
        return bottom > progressBarBottom ? bottom : progressBarBottom;
    }
}
