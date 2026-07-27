#pragma once

#include "render/UiSoundLogic.h"

namespace SH::ui_sound {
    // Safe no-op if Skyrim's audio manager or descriptor is unavailable.
    void Play(Event a_event) noexcept;
}
