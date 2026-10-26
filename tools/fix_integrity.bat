@echo off
setlocal
cd /d "%~dp0.."

echo ========================================
echo   Fix AgentForge file integrity level
echo ========================================
echo.
echo Some sandboxes (Codex / DSH / other agents) may mark files in this
echo project as "Low" integrity. Processes started from such files then
echo cannot write to Desktop / Documents ("Access denied").
echo.
echo This script raises the whole project tree to "Medium" integrity.
echo.

icacls "%CD%" /setintegritylevel "(OI)(CI)Medium" /T
if errorlevel 1 (
    echo.
    echo FAILED. Try running this file as Administrator.
    pause
    exit /b 1
)

echo.
echo Done. Restart AgentForge, then test writing to %%USERPROFILE%%\Desktop.
pause
