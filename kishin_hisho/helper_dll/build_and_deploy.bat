@echo off
REM Wrapper around build.py: build + test + deploy.
setlocal EnableExtensions

python "%~dp0..\build.py" --test --deploy %*
exit /b %errorlevel%
