@echo off
call :main > "%~dp0test.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if %errorlevel% neq 0 goto :fail
set "VCPKG_ROOT=C:\Users\Maarten\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "C:\Studios\Mod Studio\BardHero"
"%CM%" --preset release
if %errorlevel% neq 0 goto :fail
"%CM%" --build build/release --target TempoMapTests NormalizeTests ChartParserTests MidParserTests SongIniTests ScanTests UserSongWatchTests EngineTests DeterminismTests LoadSongTests ClockTests InputMapperTests HighwayLayoutTests SongCacheTests GoldScaleTests FxPoolTests SgtPerformTests StarsTests StarLedgerTests AutoPlayTests PerformTriggerTests CameraDirectorTests StreakFireTests SgtStartLeadTests EndingTests CrowdMoodTests PayoutTests UnlockTests ResultsLogicTests SongEligibilityTests BandLifecycleTests BaCompatibilityTests BaLibraryBootstrapTests PracticeTests SpFlangerTests StemSeekTests StretchTests PracticeLayoutTests WidgetMuffleTests BindingsTests NativeMenuTests
if %errorlevel% neq 0 goto :fail
build\release\TempoMapTests.exe
if %errorlevel% neq 0 goto :fail
build\release\NormalizeTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ChartParserTests.exe
if %errorlevel% neq 0 goto :fail
build\release\MidParserTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SongIniTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ScanTests.exe
if %errorlevel% neq 0 goto :fail
build\release\UserSongWatchTests.exe
if %errorlevel% neq 0 goto :fail
build\release\EngineTests.exe
if %errorlevel% neq 0 goto :fail
build\release\DeterminismTests.exe
if %errorlevel% neq 0 goto :fail
build\release\LoadSongTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ClockTests.exe
if %errorlevel% neq 0 goto :fail
build\release\InputMapperTests.exe
if %errorlevel% neq 0 goto :fail
build\release\HighwayLayoutTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SongCacheTests.exe
if %errorlevel% neq 0 goto :fail
build\release\GoldScaleTests.exe
if %errorlevel% neq 0 goto :fail
build\release\FxPoolTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SgtPerformTests.exe
if %errorlevel% neq 0 goto :fail
build\release\StarsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\StarLedgerTests.exe
if %errorlevel% neq 0 goto :fail
build\release\AutoPlayTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PerformTriggerTests.exe
if %errorlevel% neq 0 goto :fail
build\release\CameraDirectorTests.exe
if %errorlevel% neq 0 goto :fail
build\release\StreakFireTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SgtStartLeadTests.exe
if %errorlevel% neq 0 goto :fail
rem CrowdMoodTests had NO errorlevel check under it (a duplicated check sat
rem above instead) - its failures were invisible to this script.
build\release\CrowdMoodTests.exe
if %errorlevel% neq 0 goto :fail
build\release\EndingTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PayoutTests.exe
if %errorlevel% neq 0 goto :fail
build\release\UnlockTests.exe
if %errorlevel% neq 0 goto :fail
build\release\ResultsLogicTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SongEligibilityTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BandLifecycleTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BaCompatibilityTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BaLibraryBootstrapTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PracticeTests.exe
if %errorlevel% neq 0 goto :fail
build\release\SpFlangerTests.exe
if %errorlevel% neq 0 goto :fail
build\release\StemSeekTests.exe
if %errorlevel% neq 0 goto :fail
build\release\StretchTests.exe
if %errorlevel% neq 0 goto :fail
build\release\PracticeLayoutTests.exe
if %errorlevel% neq 0 goto :fail
build\release\WidgetMuffleTests.exe
if %errorlevel% neq 0 goto :fail
build\release\BindingsTests.exe
if %errorlevel% neq 0 goto :fail
build\release\NativeMenuTests.exe
if %errorlevel% neq 0 goto :fail
echo === ALL_DONE ===
exit /b 0

:fail
echo ***BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
