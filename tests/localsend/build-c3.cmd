@echo off
setlocal
set "DEVICE=%~1"
if not defined DEVICE (
  echo.
  echo LocalSend firmware build - select device:
  echo   1. X3
  echo   2. X4
  echo.
  set /p "DEVICE=Device [1/2/X3/X4]: "
)
if /I "%DEVICE%"=="1" set "DEVICE=X3"
if /I "%DEVICE%"=="2" set "DEVICE=X4"
if /I "%DEVICE%"=="X3" goto build
if /I "%DEVICE%"=="X4" goto build
echo Invalid device. Use X3 or X4.
if "%~1"=="" pause
exit /b 2
:build
echo Building LocalSend firmware for %DEVICE%...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-c3.ps1" -Device "%DEVICE%"
set "BUILD_RC=%ERRORLEVEL%"
if not "%BUILD_RC%"=="0" (
  echo.
  echo Build failed. Full output is saved under:
  echo "%~dp0out"
  pause
)
exit /b %BUILD_RC%
