@echo off
REM build_and_deploy.bat (Cross-Channel) -- thin wrapper around the game's build.py.
REM
REM The whole dev cycle (Conan install -> CMake configure -> build -> ctest
REM gate -> deploy xinput1_3.dll + translations.tsv, with the VN_DIST_BUILD
REM skip-deploy branch) lives in CROSS_CHANNEL\build.py, one level up: one
REM build script per game. Run "build.py --clean" first if you want the
REM build trees wiped beforehand.
REM
REM chcp 65001 keeps Japanese console output / CP932 paths sane.
chcp 65001 > nul
python "%~dp0..\build.py" --test --deploy %*
