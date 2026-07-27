@echo off
call :main > "%~dp0build.log" 2>&1
exit /b %errorlevel%

:main
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if %errorlevel% neq 0 goto :fail
set "VCPKG_ROOT=C:\Users\Maarten\vcpkg"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "CM=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "C:\Studios\Mod Studio\BardHero"
echo === CONFIGURE START ===
"%CM%" --preset release
if %errorlevel% neq 0 goto :fail
echo === BUILD START ===
"%CM%" --build build/release
if %errorlevel% neq 0 goto :fail
echo === TESTS START ===
rem The suite list is READ OUT OF test.bat rather than duplicated here. This
rem was a second hand-maintained roll-call and had silently fallen 17 suites
rem behind by 2026-07-26, so a build-time regression in any newer suite
rem (Practice, StemSeek, Stretch, ResultsLogic, StarLedger, BandLifecycle,
rem PracticeLayout and the rest) could not fail this script - it never ran
rem them. There is now ONE list to maintain, the one in test.bat that people
rem already update when they add a suite.
rem
rem Deliberately NOT a `build\release\*Tests.exe` glob, which looks tidier and
rem is wrong: build\release accumulates binaries from targets that have since
rem been REMOVED from CMakeLists (ControlLockTests.exe was sitting there on
rem 2026-07-26), and a glob would happily run deleted suites and report on
rem source that no longer exists.
rem
rem `if errorlevel 1` rather than `if %errorlevel% neq 0`: inside a FOR body
rem %errorlevel% is expanded ONCE when the block is parsed, so it would test
rem the value from before the loop and pass whatever the suites did.
set "RAN=0"
for /f "usebackq tokens=*" %%L in (`findstr /r /c:"^build.*Tests\.exe$" "%~dp0test.bat"`) do (
    echo --- %%L
    %%L
    if errorlevel 1 goto :fail
    set /a RAN+=1
)
rem A findstr that matched nothing would otherwise report a green run having
rem tested nothing at all - the exact failure this rewrite exists to fix.
if "%RAN%"=="0" (
    echo ***BUILD_FAILED*** no suites parsed out of test.bat
    exit /b 1
)
echo === ALL_DONE ===
exit /b 0

:fail
echo ***BUILD_FAILED*** errorlevel %errorlevel%
exit /b 1
