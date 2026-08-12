@echo off
REM build_and_deploy.bat -- thin wrapper around the game's single build driver.
REM
REM Full dev cycle for shingakkou (implemented in ..\build.py): build both
REM halves -- pipeline_cpp (x64) and this proxy DLL (Win32, static CRT via
REM profiles/x86) -- run every ctest tier, then copy winmm.dll into the game
REM folder only if the tests passed.
REM
REM The VN_DIST_BUILD env-var gate (skip the copy for build_dist.exe packaging)
REM is honored inside build.py. The driver takes --test, --install, --deploy and
REM --clean and nothing else; the game folder is baked into build.py.
chcp 65001 > nul
python "%~dp0..\build.py" --test --deploy %*
