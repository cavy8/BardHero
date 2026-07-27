#pragma once

namespace SH {
    // Filters SGT's legacy paid-lesson skill notification at the HUD message
    // boundary. SGT's scripts and compiled PEX files remain untouched.
    struct LessonNotificationHook {
        static void Install();
    };
}
