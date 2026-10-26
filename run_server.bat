@echo off
setlocal
cd /d "%~dp0"
chcp 65001 >nul

if not exist bin\Release\agentforge_web.exe (
    echo ERROR: bin\Release\agentforge_web.exe not found.
    echo Build first: quick_build.bat  or  build_and_test.ps1
    pause
    exit /b 1
)

echo ========================================
echo   AgentForge Web Server
echo ========================================
echo.
echo Web UI:  http://127.0.0.1:8080/
echo Setup:   http://127.0.0.1:8080/setup.html
echo Health:  http://127.0.0.1:8080/api/health
echo.
echo Press Ctrl+C to stop.
echo.

start "" http://127.0.0.1:8080/
bin\Release\agentforge_web.exe --config config.json
