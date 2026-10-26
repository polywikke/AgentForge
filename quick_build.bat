@echo off
setlocal
cd /d "%~dp0"
chcp 65001 >nul

echo ========================================
echo   AgentForge Quick Build
echo ========================================
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: cmake not found in PATH.
    pause
    exit /b 1
)

if not exist build mkdir build

echo [1/3] Configuring CMake...
if not exist build\CMakeCache.txt (
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 (
        echo INFO: falling back to CMake default generator...
        rmdir /s /q build
        mkdir build
        cmake -S . -B build
    )
    if errorlevel 1 (
        echo ERROR: CMake configuration failed.
        pause
        exit /b 1
    )
) else (
    echo OK: existing build cache found.
)

echo.
echo [2/3] Building Release...
cmake --build build --config Release
if errorlevel 1 (
    echo ERROR: Build failed.
    pause
    exit /b 1
)

echo.
echo [3/3] Checking output...
if exist bin\Release\agentforge_web.exe (
    echo SUCCESS: bin\Release\agentforge_web.exe
) else (
    echo ERROR: bin\Release\agentforge_web.exe not found.
    pause
    exit /b 1
)

echo.
echo ========================================
echo   Build Complete
echo ========================================
echo.
echo Press any key to start the server, or close this window to exit...
pause >nul

echo.
echo Starting AgentForge Web Server (Ctrl+C to stop)...
echo.
bin\Release\agentforge_web.exe --config config.json
