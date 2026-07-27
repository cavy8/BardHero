#include "PCH.h"

#include "LessonNotificationHook.h"

#include "game/UnlockLogic.h"

#include "RE/H/HUDData.h"
#include "RE/H/HUDMenu.h"
#include "RE/U/UIMessage.h"

namespace SH {
    namespace {
        struct HudMessageHook {
            static RE::UI_MESSAGE_RESULTS thunk(
                RE::HUDMenu* a_this, RE::UIMessage& a_message) {
                if (a_message.data) {
                    const auto* data =
                        skyrim_cast<RE::HUDData*>(a_message.data);
                    if (data &&
                        data->type == RE::HUDData::Type::kNotification) {
                        const char* text = data->text.c_str();
                        if (text &&
                            unlock::ShouldSuppressLessonNotification(text)) {
                            spdlog::info(
                                "[unlock] suppressed SGT paid-lesson "
                                "notification: \"{}\"",
                                text);
                            return RE::UI_MESSAGE_RESULTS::kHandled;
                        }
                    }
                }
                return func(a_this, a_message);
            }

            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void LessonNotificationHook::Install() {
        // HUDMenu's primary IMenu vtable; ProcessMessage is virtual slot 04
        // in CommonLibSSE. Intercepting here preserves the engine's own
        // DebugNotification function for every caller and avoids replacing
        // any SGT asset.
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
        HudMessageHook::func =
            vtbl.write_vfunc(0x04, HudMessageHook::thunk);
        spdlog::info(
            "[unlock] paid-lesson notification filter installed "
            "(HUDMenu::ProcessMessage)");
    }
}
