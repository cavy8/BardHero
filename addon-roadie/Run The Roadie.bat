@echo off
setlocal EnableExtensions
title Bard Hero - Get Songs

rem ---------------------------------------------------------------------
rem  This file is plain text, and so is the script it runs. Open either in
rem  Notepad and read it before running - you should not run a .bat from a
rem  mod on trust alone.
rem
rem  -ExecutionPolicy Bypass applies ONLY to this one script, for this one
rem  run. It changes nothing on your machine and nothing for any other
rem  program. It is here because Windows blocks unsigned .ps1 files by
rem  default, which would otherwise stop this before it started.
rem ---------------------------------------------------------------------

set "PS1=%~dp0songfetch\Get-BardHeroSongs.ps1"
if not exist "%PS1%" goto :missing

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
goto :done

:missing
echo(
echo   ERROR  Could not find:
echo              %PS1%
echo(
echo   Extract the whole download and keep this file next to the
echo   "songfetch" folder.

:done
echo(
pause
endlocal
