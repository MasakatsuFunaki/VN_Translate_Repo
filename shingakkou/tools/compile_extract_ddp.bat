@echo off
setlocal
set GARBRO=%~dp0..\..\TOOLS\garbro
set CSC=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe
set WPF=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\WPF

"%CSC%" ^
  -target:exe ^
  -out:"%GARBRO%\extract_ddp.exe" ^
  -reference:"%WPF%\PresentationCore.dll" ^
  -reference:"%WPF%\WindowsBase.dll" ^
  -reference:"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\System.Xaml.dll" ^
  "%~dp0extract_ddp.cs"

if errorlevel 1 (echo COMPILE FAILED) else (echo OK: extract_ddp.exe)
