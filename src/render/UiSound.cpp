#include "PCH.h"
#include "render/UiSound.h"

#include "RE/B/BSAudioManager.h"
#include "RE/B/BSSoundHandle.h"

namespace SH::ui_sound {
    void Play(Event a_event) noexcept {
        try {
            auto* audio = RE::BSAudioManager::GetSingleton();
            if (!audio) { return; }
            RE::BSSoundHandle handle;
            audio->BuildSoundDataFromEditorID(handle, EditorId(a_event), 0x10);
            if (handle.IsValid()) { handle.Play(); }
        } catch (...) {
            // UI feedback must never make an otherwise valid input fail.
        }
    }
}
