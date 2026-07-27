cmake_minimum_required(VERSION 3.21)

# BardHero (formerly SkyHero, renamed 2026-07-20) - Guitar-Hero-style rhythm
# minigame as an SKSE plugin (SE+AE universal DLL). Phase 1 = the engine; see
# docs/specs/2026-07-18-rhythm-engine-design.md. The display name lives ONLY
# in src/Branding.h. The SKSE co-save serialization ID stays 'SKHR' on
# purpose - changing it would orphan existing star/last-played data.
project(BardHero VERSION 0.1.0 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# libopus decoding backend (spec Q5). opus exports a CMake config; opusfile and
# its libogg dependency do NOT (vcpkg ships no config for them), so link those
# two static libs by path. Opus::opus already carries include/opus on its
# interface include dirs, which is where opusfile.h lives, so no separate
# include wiring is needed for the ma_libopus glue in MiniaudioImpl.cpp.
find_package(Opus CONFIG REQUIRED)
find_library(OPUSFILE_LIBRARY NAMES opusfile REQUIRED)
find_library(OGG_LIBRARY NAMES ogg REQUIRED)
find_library(OPUSENC_LIBRARY NAMES opusenc REQUIRED)

# --- bardcore: pure rhythm engine. No Skyrim, no D3D, no audio device, no
# windows.h (spec section 3). Anything engine/parse-related lives here so it
# runs headless; the SKSE host may link it, never the other way around.
add_library(bardcore STATIC
    bardcore/chart/TempoMap.h
    bardcore/chart/TempoMap.cpp
    bardcore/chart/ChartTypes.h
    bardcore/chart/Normalize.h
    bardcore/chart/Normalize.cpp
    bardcore/chart/ChartParser.h
    bardcore/chart/ChartParser.cpp
    bardcore/chart/Smf.h
    bardcore/chart/Smf.cpp
    bardcore/chart/MidParser.h
    bardcore/chart/MidParser.cpp
    bardcore/chart/SongIni.h
    bardcore/chart/SongIni.cpp
    bardcore/chart/Scan.h
    bardcore/chart/Scan.cpp
    bardcore/chart/LoadSong.h
    bardcore/chart/LoadSong.cpp
    bardcore/engine/InputTypes.h
    bardcore/engine/EngineParams.h
    bardcore/engine/GuitarEngine.h
    bardcore/engine/GuitarEngine.cpp
    bardcore/replay/Replay.h
    bardcore/replay/Replay.cpp
    bardcore/clock/MasterClock.h
    bardcore/clock/MasterClock.cpp
    bardcore/clock/ResumeCountdown.h
    bardcore/clock/AnchorSeqlock.h
    bardcore/clock/AnchorEstimator.h
    bardcore/clock/AnchorEstimator.cpp
    bardcore/clock/SlaveController.h
    bardcore/clock/SlaveController.cpp
    bardcore/practice/PracticeSections.h
    bardcore/practice/PracticeSections.cpp
    bardcore/practice/PracticeSlice.h
    bardcore/practice/PracticeSlice.cpp
    bardcore/practice/PracticeRange.h
    bardcore/practice/PracticeLoop.h)
target_compile_features(bardcore PUBLIC cxx_std_23)
target_include_directories(bardcore PUBLIC bardcore)

# --- bardcore test exes (headless; run by tools/build.bat and tools/test.bat)
add_executable(TempoMapTests tests/test_tempomap.cpp)
target_link_libraries(TempoMapTests PRIVATE bardcore)
target_compile_features(TempoMapTests PRIVATE cxx_std_23)

add_executable(NormalizeTests tests/test_normalize.cpp)
target_link_libraries(NormalizeTests PRIVATE bardcore)
target_compile_features(NormalizeTests PRIVATE cxx_std_23)

add_executable(ChartParserTests tests/test_chartparser.cpp)
target_link_libraries(ChartParserTests PRIVATE bardcore)
target_compile_features(ChartParserTests PRIVATE cxx_std_23)

add_executable(MidParserTests tests/test_midparser.cpp)
target_link_libraries(MidParserTests PRIVATE bardcore)
target_compile_features(MidParserTests PRIVATE cxx_std_23)

add_executable(SongIniTests tests/test_songini.cpp)
target_link_libraries(SongIniTests PRIVATE bardcore)
target_compile_features(SongIniTests PRIVATE cxx_std_23)

add_executable(ScanTests tests/test_scan.cpp)
target_link_libraries(ScanTests PRIVATE bardcore)
target_compile_features(ScanTests PRIVATE cxx_std_23)

add_executable(UserSongWatchTests tests/test_usersongwatch.cpp)
target_include_directories(UserSongWatchTests PRIVATE src tests)
target_compile_features(UserSongWatchTests PRIVATE cxx_std_23)

add_executable(EngineTests tests/test_engine.cpp)
target_link_libraries(EngineTests PRIVATE bardcore)
target_compile_features(EngineTests PRIVATE cxx_std_23)

add_executable(DeterminismTests tests/test_determinism.cpp)
target_link_libraries(DeterminismTests PRIVATE bardcore)
target_compile_features(DeterminismTests PRIVATE cxx_std_23)

add_executable(LoadSongTests tests/test_loadsong.cpp)
target_link_libraries(LoadSongTests PRIVATE bardcore)
target_compile_features(LoadSongTests PRIVATE cxx_std_23)

add_executable(SongLoadProbe tools/song_load_probe.cpp)
target_include_directories(SongLoadProbe PRIVATE src)
target_link_libraries(SongLoadProbe PRIVATE bardcore)
target_compile_features(SongLoadProbe PRIVATE cxx_std_23)

add_executable(ClockTests tests/test_clock.cpp)
target_link_libraries(ClockTests PRIVATE bardcore)
target_compile_features(ClockTests PRIVATE cxx_std_23)

# InputMapper is host-side but OS-free pure logic - the one src/ unit the
# headless suites can and must cover (M0: its filters are REQUIRED design
# elements). Compiled directly into the test exe; bardcore provides
# engine/InputTypes.h via its PUBLIC include dir.
add_executable(InputMapperTests tests/test_inputmapper.cpp src/game/InputMapper.cpp)
target_link_libraries(InputMapperTests PRIVATE bardcore)
target_include_directories(InputMapperTests PRIVATE src)
target_compile_features(InputMapperTests PRIVATE cxx_std_23)

# HighwayLayout is host-side but OS-free pure geometry (spec 9 seam) - the
# second src/ unit the headless suites cover (InputMapper pattern).
add_executable(HighwayLayoutTests tests/test_highwaylayout.cpp src/render/HighwayLayout.cpp)
target_link_libraries(HighwayLayoutTests PRIVATE bardcore)
target_include_directories(HighwayLayoutTests PRIVATE src)
target_compile_features(HighwayLayoutTests PRIVATE cxx_std_23)

add_executable(SongCacheTests tests/test_songcache.cpp src/game/SongCache.cpp)
target_link_libraries(SongCacheTests PRIVATE bardcore)
target_include_directories(SongCacheTests PRIVATE src)
target_compile_features(SongCacheTests PRIVATE cxx_std_23)

# GoldScaleMath is host-side but pure std (header-only) - third src/ unit
# under headless test (InputMapper pattern).
add_executable(GoldScaleTests tests/test_goldscale.cpp)
target_include_directories(GoldScaleTests PRIVATE src)
target_compile_features(GoldScaleTests PRIVATE cxx_std_23)

# FxPool is pure particle math (no RE/FUCK/OS) - fourth src/ unit under
# headless test.
add_executable(FxPoolTests tests/test_fxpool.cpp src/render/FxPool.cpp)
target_include_directories(FxPoolTests PRIVATE src)
target_compile_features(FxPoolTests PRIVATE cxx_std_23)

# SgtPerformLogic is pure state logic (no RE/OS) - fifth src/ unit under
# headless test.
add_executable(SgtPerformTests tests/test_sgtperform.cpp)
target_include_directories(SgtPerformTests PRIVATE src)
target_compile_features(SgtPerformTests PRIVATE cxx_std_23)

# StarsLogic is pure math/state (no RE/OS) - sixth src/ unit under
# headless test.
add_executable(StarsTests tests/test_stars.cpp)
target_include_directories(StarsTests PRIVATE src)
target_compile_features(StarsTests PRIVATE cxx_std_23)

# StarLedgerCore is a pure ledger + byte-buffer serialization (no RE/OS) -
# seventh src/ unit under headless test.
add_executable(StarLedgerTests tests/test_starledger.cpp
               src/game/StarLedgerCore.cpp)
target_include_directories(StarLedgerTests PRIVATE src)
target_compile_features(StarLedgerTests PRIVATE cxx_std_23)

# AutoPlayBot is a pure header-only feeder (no RE/OS) - eighth src/ unit
# under headless test; proven against the real engine.
add_executable(AutoPlayTests tests/test_autoplay.cpp)
target_link_libraries(AutoPlayTests PRIVATE bardcore)
target_include_directories(AutoPlayTests PRIVATE src)
target_compile_features(AutoPlayTests PRIVATE cxx_std_23)

# PerformTriggerLogic arbitrates the two trigger sources (the synchronous
# AddTarget hook and the 500ms poll belt) - pure header, no RE/OS, ninth
# src/ unit under headless test.
add_executable(PerformTriggerTests tests/test_performtrigger.cpp)
target_include_directories(PerformTriggerTests PRIVATE src)
target_compile_features(PerformTriggerTests PRIVATE cxx_std_23)

# The performance camera director's pure layer: cut cadence, minimum hold,
# glory weighting, occlusion blacklist and beat-locked shake. Spec
# docs/superpowers/specs/2026-07-26-performance-camera-director-design.md,
# which requires this suite alongside the header. Pure, no RE/OS.
# Streak fire: WHEN the player's hands catch light as a note streak builds,
# and when they go out. Pure, no RE/OS - the art itself is Session.cpp glue.
add_executable(StreakFireTests tests/test_streakfire.cpp)
target_include_directories(StreakFireTests PRIVATE src)
target_compile_features(StreakFireTests PRIVATE cxx_std_23)

# Widget muffle: WHICH third-party HUD widget menus go dark during a
# session. Pure list parsing - the UI work is WidgetMuffle.cpp glue.
add_executable(WidgetMuffleTests tests/test_widgetmuffle.cpp)
target_include_directories(WidgetMuffleTests PRIVATE src)
target_compile_features(WidgetMuffleTests PRIVATE cxx_std_23)

# Bindings tab: key/button names for both code spaces and the
# steal-on-conflict + defaults logic. Links bardcore for the
# engine/InputTypes.h include that InputMapper.h (the defaults source)
# pulls in - fields only, so InputMapper.cpp itself is not compiled here.
add_executable(BindingsTests tests/test_bindings.cpp)
target_link_libraries(BindingsTests PRIVATE bardcore)
target_include_directories(BindingsTests PRIVATE src)
target_compile_features(BindingsTests PRIVATE cxx_std_23)

# Native-menu recovery classification: which menus count as "owns the
# player" (drives session recovery AND the browse-phase yield to forced
# dialogue - the guard-arrest deadlock, field 2026-07-27).
add_executable(NativeMenuTests tests/test_nativemenu.cpp)
target_include_directories(NativeMenuTests PRIVATE src)
target_compile_features(NativeMenuTests PRIVATE cxx_std_23)

add_executable(CameraDirectorTests tests/test_cameradirector.cpp)
target_include_directories(CameraDirectorTests PRIVATE src)
target_compile_features(CameraDirectorTests PRIVATE cxx_std_23)

# SgtStartLead sizes the audio lead-in against SGT's own start wind-up so
# the first note never lands before the character is playing - pure header,
# no RE/OS, tenth src/ unit under headless test.
add_executable(SgtStartLeadTests tests/test_sgtstartlead.cpp)
target_include_directories(SgtStartLeadTests PRIVATE src)
target_compile_features(SgtStartLeadTests PRIVATE cxx_std_23)

# EndingLogic decides the end-of-performance reaction, message and XP from
# the RUN rather than SGT's rank+dice - pure header, no RE/OS.
add_executable(EndingTests tests/test_ending.cpp)
target_include_directories(EndingTests PRIVATE src)
target_compile_features(EndingTests PRIVATE cxx_std_23)

# CrowdMoodLogic is the pure rolling-window mood model (no RE/OS) - under
# headless test so the whole feature can be built without the game.
add_executable(CrowdMoodTests tests/test_crowdmood.cpp)
target_include_directories(CrowdMoodTests PRIVATE src)
target_compile_features(CrowdMoodTests PRIVATE cxx_std_23)

# PayoutMath is pure gold maths (no RE/OS) - under headless test.
add_executable(PayoutTests tests/test_payout.cpp)
target_include_directories(PayoutTests PRIVATE src)
target_compile_features(PayoutTests PRIVATE cxx_std_23)

# UnlockLogic is the pure song-unlock gate (no RE/OS) - headless test.
add_executable(UnlockTests tests/test_unlock.cpp)
target_include_directories(UnlockTests PRIVATE src)
target_compile_features(UnlockTests PRIVATE cxx_std_23)

# ResultsLogic is the pure HUD/results presentation maths - live accuracy,
# star bands, the rank ladder and the gold explanation (no RE/OS).
add_executable(ResultsLogicTests tests/test_resultslogic.cpp)
target_include_directories(ResultsLogicTests PRIVATE src)
target_compile_features(ResultsLogicTests PRIVATE cxx_std_23)

# SongEligibility freezes an SGT instrument into the Songbook and owns both
# row filtering and final Play authorization. Pure header, no RE/OS.
add_executable(SongEligibilityTests tests/test_songeligibility.cpp)
target_include_directories(SongEligibilityTests PRIVATE src tests bardcore)
target_compile_features(SongEligibilityTests PRIVATE cxx_std_23)

add_executable(BandLifecycleTests tests/test_bandlifecycle.cpp)
target_include_directories(BandLifecycleTests PRIVATE src)
target_compile_features(BandLifecycleTests PRIVATE cxx_std_23)

add_executable(BaCompatibilityTests tests/test_bacompatibility.cpp)
target_include_directories(BaCompatibilityTests PRIVATE src tests)
target_compile_features(BaCompatibilityTests PRIVATE cxx_std_23)

add_executable(BaLibraryBootstrapTests
    tests/test_balibrarybootstrap.cpp
    src/game/BaLibraryBootstrap.cpp
    src/audio/XwmTranscoder.cpp)
target_include_directories(BaLibraryBootstrapTests PRIVATE src tests)
target_compile_features(BaLibraryBootstrapTests PRIVATE cxx_std_23)
target_link_libraries(BaLibraryBootstrapTests PRIVATE
    bardcore spdlog::spdlog Opus::opus "${OPUSENC_LIBRARY}"
    mfplat mfreadwrite mfuuid wmcodecdspuuid ole32)

add_executable(PracticeTests tests/test_practice.cpp)
target_link_libraries(PracticeTests PRIVATE bardcore)
target_compile_features(PracticeTests PRIVATE cxx_std_23)

add_executable(SpFlangerTests tests/test_spflanger.cpp)
target_include_directories(SpFlangerTests PRIVATE src tests)
target_compile_features(SpFlangerTests PRIVATE cxx_std_23)

# StemSeekLogic is the pure frame arithmetic behind AudioEngine::SeekStems -
# the practice loop's backwards seek. Split out of the engine because
# AudioEngine owns a live miniaudio device and cannot run headless; the
# header itself pulls in nothing but <cstdint>.
add_executable(StemSeekTests tests/test_stemseek.cpp)
target_include_directories(StemSeekTests PRIVATE src tests)
target_compile_features(StemSeekTests PRIVATE cxx_std_23)

# signalsmith-stretch smoke test. extern/signalsmith is the single include dir
# the vendored chain needs: signalsmith-stretch.h includes
# "signalsmith-linear/stft.h" and that includes "./fft.h", both of which
# resolve under it. Deliberately NOT linked to bardcore - bardcore stays free
# of audio libraries.
add_executable(StretchTests tests/test_stretch.cpp)
target_include_directories(StretchTests PRIVATE tests extern/signalsmith)
target_compile_features(StretchTests PRIVATE cxx_std_23)

# Practice section picker geometry + selection (plan P4). Header-only and
# free of ImGui/FUCK/RE, so it tests headlessly like GoldScale - including
# the scroll-window arithmetic that stands in for FUCK::SetScrollHereY,
# which is version-2 gated and silently no-ops on a v1 host.
add_executable(PracticeLayoutTests tests/test_practicelayout.cpp)
target_include_directories(PracticeLayoutTests PRIVATE src tests)
target_compile_features(PracticeLayoutTests PRIVATE cxx_std_23)

add_executable(XwmTranscoderProbe
    tools/xwm_transcode_probe.cpp
    src/audio/XwmTranscoder.cpp)
target_include_directories(XwmTranscoderProbe PRIVATE src)
target_compile_features(XwmTranscoderProbe PRIVATE cxx_std_23)
target_link_libraries(XwmTranscoderProbe PRIVATE
    Opus::opus "${OPUSENC_LIBRARY}"
    mfplat mfreadwrite mfuuid wmcodecdspuuid ole32)

add_executable(BaBootstrapProbe
    tools/ba_bootstrap_probe.cpp
    src/game/BaLibraryBootstrap.cpp
    src/audio/XwmTranscoder.cpp)
target_include_directories(BaBootstrapProbe PRIVATE src bardcore)
target_compile_features(BaBootstrapProbe PRIVATE cxx_std_23)
target_link_libraries(BaBootstrapProbe PRIVATE
    spdlog::spdlog Opus::opus "${OPUSENC_LIBRARY}"
    mfplat mfreadwrite mfuuid wmcodecdspuuid ole32)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/PCH.h
        src/Branding.h
        src/plugin.cpp
        src/Settings.h
        src/Settings.cpp
        src/MiniaudioImpl.cpp
        src/QpcClock.h
        src/audio/AudioEngine.h
        src/audio/AudioEngine.cpp
        src/audio/XwmTranscoder.h
        src/audio/XwmTranscoder.cpp
        src/spikes/Spike2Audio.h
        src/spikes/Spike2Audio.cpp
        src/spikes/Spike3Render.h
        src/spikes/Spike3Render.cpp
        src/game/Ducking.h
        src/game/Ducking.cpp
        src/game/EngineFeed.h
        src/game/UiBus.h
        src/game/DifficultyTuning.h
        src/game/SongCache.h
        src/game/SongCache.cpp
        src/game/SongLibrary.h
        src/game/SongLibrary.cpp
        src/game/UserSongWatchLogic.h
        src/game/BaCompatibility.h
        src/game/BaLibraryBootstrap.h
        src/game/BaLibraryBootstrap.cpp
        src/game/SongChallengeLogic.h
        src/game/SongEligibility.h
        src/game/FlickOpenGate.h
        src/render/AtlasUv.h
        src/render/IHighwayRenderer.h
        src/render/FxPool.h
        src/render/FxPool.cpp
        src/render/HighwayLayout.h
        src/render/HighwayLayout.cpp
        src/render/FlickRenderer.h
        src/render/FlickRenderer.cpp
        src/render/HighwayWindow.h
        src/render/HighwayWindow.cpp
        src/render/FlickWindowPolicy.h
        src/render/BackdropLayout.h
        src/render/HudWindow.h
        src/render/HudWindow.cpp
        src/render/RenderUi.h
        src/render/RenderUi.cpp
        src/render/PanelStyleLogic.h
        src/render/SongSortLogic.h
        src/render/UiSoundLogic.h
        src/render/UiSound.h
        src/render/UiSound.cpp
        src/render/PauseMenuWindow.h
        src/render/PauseMenuWindow.cpp
        src/render/ResultsWindow.h
        src/render/ResultsWindow.cpp
        src/render/BrowserWindow.h
        src/render/BrowserWindow.cpp
        src/render/SettingsTool.h
        src/render/SettingsTool.cpp
        src/game/AutoPlayBot.h
        src/game/BrowseCameraLogic.h
        src/game/InputMapper.h
        src/game/InputMapper.cpp
        src/game/PauseInputLogic.h
        src/game/ListNavigationLogic.h
        src/game/SessionInputLogic.h
        src/game/NativeMenuLogic.h
        src/game/InputHook.h
        src/game/InputHook.cpp
        src/game/LessonNotificationHook.h
        src/game/LessonNotificationHook.cpp
        src/game/Session.h
        src/game/Session.cpp
        src/game/BandLifecycleLogic.h
        src/game/BandFormationLogic.h
        src/game/BandPerformanceLogic.h
        src/game/BandStage.h
        src/game/BandStage.cpp
        src/game/WidgetMuffleLogic.h
        src/game/WidgetMuffle.h
        src/game/WidgetMuffle.cpp
        src/game/ShadowPause.h
        src/game/ShadowPause.cpp
        src/game/MovementGuard.h
        src/game/MovementGuard.cpp
        src/game/GoldScaleMath.h
        src/game/GoldScale.h
        src/game/GoldScale.cpp
        src/game/SgtPerformLogic.h
        src/game/SgtVm.h
        src/game/SgtVm.cpp
        src/game/PerformTriggerHook.h
        src/game/PerformTriggerHook.cpp
        src/game/PerformanceCamera.h
        src/game/PerformanceCamera.cpp
        src/game/StarsLogic.h
        src/game/StarLedgerCore.h
        src/game/StarLedgerCore.cpp
        src/game/StarLedger.h
        src/game/StarLedger.cpp
        src/game/SgtProgression.h
        src/game/SgtProgression.cpp
        src/game/MoodGlobals.h
        src/game/MoodGlobals.cpp
        src/game/AtronachRecipe.h
        src/game/AtronachRecipe.cpp
        src/game/CrowdReactions.h
        src/game/CrowdReactions.cpp
        src/game/UiSfxLogic.h
        src/game/UiSfx.h
        src/game/UiSfx.cpp
        src/game/ResultsLogic.h)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# extern/ holds the vendored FUCK API header (soft dependency, resolved at
# runtime via RequestFUCK - no link-time coupling) and, from the audio spike
# on, miniaudio + stb_vorbis. extern/signalsmith is listed as its own include
# dir so the vendored signalsmith-stretch.h can be included by its own name,
# the spelling its internal "signalsmith-linear/stft.h" include expects.
target_include_directories(${PROJECT_NAME} PRIVATE src extern extern/signalsmith)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui bardcore
    Opus::opus "${OPUSFILE_LIBRARY}" "${OGG_LIBRARY}" "${OPUSENC_LIBRARY}"
    mfplat mfreadwrite mfuuid wmcodecdspuuid ole32)
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

if(NOT DEFINED OUTPUT_FOLDER)
    set(OUTPUT_FOLDER "C:/Games/Nolvus/Instances/Nolvus Awakening/MODS/mods/BardHero")
endif()
# Debug symbols in Release. Every BardHero CTD so far has been unsymbolisable
# - trainwreck prints "BardHero.dll+00B8DA7" and nothing else - which forced
# crash diagnosis from register values alone and cost two wrong guesses on
# 2026-07-20. /Zi emits the debug info and /DEBUG makes the linker write the
# PDB; /OPT:REF and /OPT:ICF put back the two optimisations that /DEBUG turns
# off by default, so the shipped code stays equivalent to a plain Release
# build. The PDB is deliberately NOT deployed - it stays in build/release and
# is resolved offline with Tools/re/pdb_syms.py, so it can never wander into
# a release zip.
target_compile_options(${PROJECT_NAME} PRIVATE /Zi)
target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)

set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
message(STATUS "BardHero deploy folder: ${DLL_FOLDER}")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/dist" "${OUTPUT_FOLDER}"
    VERBATIM)
