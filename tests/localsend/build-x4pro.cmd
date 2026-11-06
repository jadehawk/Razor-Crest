@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-x4pro.ps1"
set "BUILD_RC=%ERRORLEVEL%"
if not "%BUILD_RC%"=="0" (
  echo.
  echo Build failed. Full output is saved under:
  echo "%~dp0out"
  echo.
  pause
)
exit /b %BUILD_RC%
