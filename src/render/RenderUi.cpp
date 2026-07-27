// src/render/RenderUi.cpp
#include "PCH.h"
#include "render/RenderUi.h"

#include "render/BrowserWindow.h"
#include "render/HighwayWindow.h"
#include "render/HudWindow.h"
#include "render/PauseMenuWindow.h"
#include "render/ResultsWindow.h"
#include "render/SettingsTool.h"

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h (house ordering)

#include "FUCK_API.h"

namespace SH::RenderUi {
    namespace {
        int g_cursorRefs = 0;  // render thread only
    }

    void Register() {
        RegisterHighwayWindow();
        RegisterHudWindow();
        RegisterResultsWindow();
        RegisterBrowserWindow();
        RegisterPauseMenuWindow();
        RegisterSettingsTool();  // FLICK sidebar entry (ITool, not a window)
        spdlog::info("[render] M4 windows registered "
                     "(results kPassInputToGame=ON).");
    }

    void AcquireCursor() {
        if (g_cursorRefs++ == 0) FUCK::ForceCursor(true);
    }
    void ReleaseCursor() {
        if (g_cursorRefs > 0 && --g_cursorRefs == 0) FUCK::ForceCursor(false);
    }
    int CursorRefs() { return g_cursorRefs; }
}
