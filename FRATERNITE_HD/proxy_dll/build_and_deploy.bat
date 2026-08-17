@echo off
REM Wrapper around build.py: build + test + deploy.
chcp 65001 > nul
python "%~dp0..\build.py" --test --deploy %*
exit /b %errorlevel%
