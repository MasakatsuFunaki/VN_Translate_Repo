@echo off
REM build_and_deploy.bat -- thin wrapper around the game's single build driver.
REM
REM A developer convenience, not part of the pipeline: 00_run_all copies the
REM DLL itself rather than calling this, because a rebuild launched from a
REM running 00_run_all.exe would relink the binary that started it.
REM
REM All the real logic lives in mushigurui_HD10\build.py, which builds both
REM halves (pipeline_cpp x64 and proxy_dll Win32), runs every test tier as a
REM deploy gate, and copies winmm.dll to C:\Games\mushigurui_HD10.
REM VN_DIST_BUILD=1 skips the copy.
REM
REM Extra args pass straight through.
python "%~dp0..\build.py" --test --deploy %*
