@echo off
chcp 65001 >nul
set PYTHONUTF8=1
set PYTHONIOENCODING=utf-8
set CLAUDE_CODE_USE_POWERSHELL_TOOL=1
if "%~1"=="" (
  claude --dangerously-skip-permissions
) else (
  claude --dangerously-skip-permissions --resume %1
)
