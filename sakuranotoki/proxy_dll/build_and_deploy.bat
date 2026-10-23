@echo off
REM Wrapper around build.py: build + test + deploy.
python "%~dp0..\build.py" --test --deploy %*
