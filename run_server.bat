@echo off
rem ============================================================================
rem  AgentForge Web Server
rem
rem  Starts bin\Release\agentforge_web.exe and opens the Web UI in the browser.
rem  The first run (no API key yet) opens the configuration wizard instead.
rem
rem  Usage:
rem    run_server.bat                 start on the configured port (8080)
rem    run_server.bat --port 9000     start on another port
rem    run_server.bat --no-open       do not open the browser
rem ============================================================================
setlocal EnableExtensions
cd /d "%~dp0"
chcp 65001 >nul
title AgentForge Web Server

set "EXE=bin\Release\agentforge_web.exe"

if exist "%EXE%" goto :start

echo [info] %EXE% was not found - building the project first ...
echo.
call "%~dp0quick_build.bat" --no-pause
if errorlevel 1 goto :build_failed

:start
if not exist "%EXE%" goto :build_failed

echo.
echo ============================================================
echo   AgentForge Web Server
echo ============================================================
echo   The browser will open automatically when the server is ready.
echo   First run: fill in API Key / endpoint / model in the wizard.
echo.
echo   Press Ctrl+C to stop the server.
echo ============================================================
echo.

"%EXE%" --config "config.json" %*
if errorlevel 1 goto :server_failed
exit /b 0

:build_failed
echo.
echo ERROR: the project could not be built, so the server cannot start.
echo        Run quick_build.bat and read the messages above.
echo.
pause
exit /b 1

:server_failed
echo.
echo ERROR: the server stopped unexpectedly (see messages above).
echo        If the port is busy the server picks the next free one - check
echo        the "Web UI" line printed above.
echo.
pause
exit /b 1
