@echo off
REM build_and_deploy.bat -- thin wrapper around the game's build.py.
REM
REM The whole dev cycle (Conan install -> CMake configure -> build -> ctest
REM gate -> deploy winmm.dll, with the VN_DIST_BUILD skip-deploy branch)
REM lives in amakano2pe\build.py, one level up: one build script per game.
REM Run "build.py --clean" first if you want the build trees wiped beforehand.
REM
REM The game folder has no default; pass it through:
REM     build_and_deploy.bat --game-dir "C:\...\amakano2 install dir"
REM
REM chcp 65001 keeps Japanese console output / CP932 paths sane.
chcp 65001 > nul
python "%~dp0..\build.py" --test --deploy %*
