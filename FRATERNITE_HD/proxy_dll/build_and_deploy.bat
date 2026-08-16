@echo off
REM build_and_deploy.bat -- thin wrapper around the game's single build driver.
REM
REM One-shot dev cycle for FRATERNITE_HD:
REM   pipeline_cpp (x64) + proxy_dll (Win32) -> ctest -> copy winmm.dll to the
REM   game folder. Tests gate the deploy.
REM
REM All logic lives in ..\build.py. This wrapper only:
REM   - sets the console code page to UTF-8 so JP log text prints correctly,
REM   - asks for a tested deploy,
REM   - forwards any extra args through.
REM
REM VN_DIST_BUILD is honored by build.py (skips the deploy step).
REM Prereqs on PATH: python, cmake, ctest, ninja, conan (>=2.0), and the MSVC
REM C++ toolchain from VS 2022.

chcp 65001 > nul

python "%~dp0..\build.py" --test --deploy %*
exit /b %errorlevel%
