@echo off
REM build_and_deploy.bat  (EXTRAVAGANZA_MATSURO_CE)
REM
REM Thin wrapper -- the real logic lives in the game's single build driver,
REM ..\build.py, which builds both halves (pipeline_cpp x64, proxy_dll Win32).
REM One-shot dev cycle: build -> ctest gating (abort on failure) -> deploy
REM winmm.dll to the game dir.  VN_DIST_BUILD is honoured inside build.py
REM (deploy is skipped when set).
python "%~dp0..\build.py" --test --deploy %*
