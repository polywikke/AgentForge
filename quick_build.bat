@echo off
rem ============================================================================
rem  AgentForge Quick Build
rem
rem  Double-click this file (no Developer Command Prompt needed).
rem  It searches for CMake / Visual Studio / MinGW by itself and builds
rem  bin\Release\agentforge_web.exe (the Web UI server).
rem
rem  Usage:
rem    quick_build.bat              build, then pause
rem    quick_build.bat --no-pause   build without pause (used by run_server.bat)
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
chcp 65001 >nul
title AgentForge Quick Build

set "ROOT=%CD%"
set "BUILD_DIR=%ROOT%\build"
set "EXE=%ROOT%\bin\Release\agentforge_web.exe"
set "CONFIG_LOG=%TEMP%\agentforge-configure.log"

echo.
echo ============================================================
echo   AgentForge Quick Build
echo ============================================================
echo   Project : %ROOT%
echo ============================================================
echo.

rem ---------------------------------------------------------------------------
rem 0) sanity: are we inside the project folder?
rem ---------------------------------------------------------------------------
if not exist "%ROOT%\CMakeLists.txt" goto :err_no_source
if not exist "%ROOT%\include\httplib.h" goto :err_no_source
if not exist "%ROOT%\include\nlohmann\json.hpp" goto :err_no_source
if not exist "%ROOT%\thirdparty\tree-sitter\core\src\lib.c" goto :err_no_source

rem ---------------------------------------------------------------------------
rem 1) locate CMake (PATH -> CMake bundled with Visual Studio -> common paths)
rem ---------------------------------------------------------------------------
call :find_cmake
if not defined CMAKE_EXE goto :err_no_cmake
for /f "delims=" %%v in ('"%CMAKE_EXE%" --version 2^>nul') do if not defined CMAKE_VER set "CMAKE_VER=%%v"
echo [Tool] !CMAKE_VER!

rem ---------------------------------------------------------------------------
rem 2) which compilers are available? (used for the fallback generators)
rem ---------------------------------------------------------------------------
call :find_compilers

rem ---------------------------------------------------------------------------
rem 3) reuse a valid build cache, otherwise configure from scratch
rem ---------------------------------------------------------------------------
set "REUSED_CACHE="
call :cache_is_usable
if not defined CACHE_OK goto :need_configure

echo [1/3] Reusing build cache in build\ (incremental build)
echo       (delete the build folder if you want a full reconfigure)
set "REUSED_CACHE=1"
goto :do_build

:need_configure
echo [1/3] Configuring CMake (first run may take a little while)...
call :choose_generator
if not defined CONFIGURED goto :err_configure_multi

:do_build
echo.
echo [2/3] Building Release ...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --parallel
if not errorlevel 1 goto :verify

if defined REUSED_CACHE (
    echo.
    echo [info] Build failed with the old cache - reconfiguring from scratch ...
    set "REUSED_CACHE="
    call :choose_generator
    if not defined CONFIGURED goto :err_configure_multi
    echo.
    echo [2/3] Building Release (retry) ...
    "%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --parallel
    if not errorlevel 1 goto :verify
)
goto :err_build

:verify
echo.
echo [3/3] Verifying output ...
if not exist "%EXE%" goto :err_no_exe

call :copy_runtime_dlls

for %%f in ("%EXE%") do set "EXE_SIZE=%%~zf"
echo.
echo ============================================================
echo   Build Complete
echo ============================================================
echo   Generator : %GEN_LABEL%
echo   Output    : %EXE%
echo   Size      : !EXE_SIZE! bytes
echo.
echo   Next step : run_server.bat
echo   (it starts the server and opens the browser; the very first run
echo    shows the API configuration wizard)
echo ============================================================
echo.
if /i "%~1"=="--no-pause" exit /b 0
pause
exit /b 0

rem ===========================================================================
rem Subroutines
rem ===========================================================================

:find_cmake
set "CMAKE_EXE="
for /f "delims=" %%i in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
if defined CMAKE_EXE exit /b 0

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do (
        if not defined CMAKE_EXE if exist "%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "CMAKE_EXE=%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
    )
)
if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles(x86)%\CMake\bin\cmake.exe"
if not defined CMAKE_EXE if exist "%LOCALAPPDATA%\Programs\CMake\bin\cmake.exe" set "CMAKE_EXE=%LOCALAPPDATA%\Programs\CMake\bin\cmake.exe"
exit /b 0

:find_compilers
set "GXX="
set "NINJA="
set "MINGW_MAKE="
for /f "delims=" %%i in ('where g++ 2^>nul') do if not defined GXX set "GXX=%%i"
for /f "delims=" %%i in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%i"
for /f "delims=" %%i in ('where mingw32-make 2^>nul') do if not defined MINGW_MAKE set "MINGW_MAKE=%%i"
if defined GXX echo [Tool] g++   : !GXX!
if defined NINJA echo [Tool] Ninja : !NINJA!
exit /b 0

:cache_is_usable
set "CACHE_OK="
if not exist "%BUILD_DIR%\CMakeCache.txt" exit /b 0

set "CACHE_HOME="
for /f "tokens=1,* delims==" %%a in ('findstr /b /c:"CMAKE_HOME_DIRECTORY:INTERNAL=" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do set "CACHE_HOME=%%b"
if not defined CACHE_HOME exit /b 0
set "CACHE_HOME=!CACHE_HOME:/=\!"
if /i not "!CACHE_HOME!"=="%ROOT%" (
    echo [info] build\ was created in another folder: !CACHE_HOME!
    echo [info] deleting the stale cache ...
    rmdir /s /q "%BUILD_DIR%" >nul 2>&1
    exit /b 0
)

set "CACHE_GEN="
for /f "tokens=1,* delims==" %%a in ('findstr /b /c:"CMAKE_GENERATOR:INTERNAL=" "%BUILD_DIR%\CMakeCache.txt" 2^>nul') do set "CACHE_GEN=%%b"
if not defined CACHE_GEN exit /b 0
"%CMAKE_EXE%" --help 2>nul | findstr /c:"!CACHE_GEN!" >nul
if errorlevel 1 (
    echo [info] generator "!CACHE_GEN!" is no longer available - rebuilding configuration
    rmdir /s /q "%BUILD_DIR%" >nul 2>&1
    exit /b 0
)
set "GEN_LABEL=!CACHE_GEN!"
set "CACHE_OK=1"
exit /b 0

:choose_generator
set "CONFIGURED="
set "GEN_LABEL="
call :try_configure "Visual Studio 18 2026" "-A x64"
if defined CONFIGURED exit /b 0
call :try_configure "Visual Studio 17 2022" "-A x64"
if defined CONFIGURED exit /b 0
call :try_configure "Visual Studio 16 2019" "-A x64"
if defined CONFIGURED exit /b 0
call :try_configure "Visual Studio 15 2017" "-A x64"
if defined CONFIGURED exit /b 0
if defined GXX if defined NINJA (
    call :try_configure "Ninja" ""
    if defined CONFIGURED exit /b 0
)
if defined GXX if defined MINGW_MAKE (
    call :try_configure "MinGW Makefiles" ""
    if defined CONFIGURED exit /b 0
)
call :try_configure "" ""
exit /b 0

:try_configure
set "GEN=%~1"
set "GEN_ARGS=%~2"
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%" >nul 2>&1
if exist "%BUILD_DIR%" (
    echo   [warn] cannot remove %BUILD_DIR% - close Visual Studio or a running build first
    exit /b 1
)
if not defined GEN (
    echo   [try] default generator ...
    "%CMAKE_EXE%" -S "%ROOT%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release > "%CONFIG_LOG%" 2>&1
    goto :configure_result
)
echo   [try] %GEN% ...
echo %GEN% | findstr /i /c:"Visual Studio" >nul
if errorlevel 1 (
    "%CMAKE_EXE%" -S "%ROOT%" -B "%BUILD_DIR%" -G "%GEN%" %GEN_ARGS% -DCMAKE_BUILD_TYPE=Release > "%CONFIG_LOG%" 2>&1
) else (
    "%CMAKE_EXE%" -S "%ROOT%" -B "%BUILD_DIR%" -G "%GEN%" %GEN_ARGS% > "%CONFIG_LOG%" 2>&1
)
:configure_result
if errorlevel 1 (
    echo   [skip] not usable with the installed compiler
    exit /b 1
)
set "CONFIGURED=1"
set "GEN_LABEL=%GEN%"
if not defined GEN_LABEL set "GEN_LABEL=default"
exit /b 0

:copy_runtime_dlls
rem MinGW builds need their C++ runtime DLLs next to the exe
if defined EXE_COPIED exit /b 0
if not defined GXX exit /b 0
echo %GEN_LABEL% | findstr /i /c:"Visual Studio" >nul
if not errorlevel 1 exit /b 0
for %%d in ("%GXX%") do set "GCC_BIN=%%~dpd"
for %%f in (libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll) do (
    if exist "%GCC_BIN%%%f" copy /y "%GCC_BIN%%%f" "%ROOT%\bin\Release\" >nul 2>&1
)
set "EXE_COPIED=1"
exit /b 0

rem ===========================================================================
rem Errors
rem ===========================================================================

:err_no_source
echo ERROR: this script must stay inside the AgentForge folder.
echo        Missing CMakeLists.txt / include\ / thirdparty\.
echo.
if /i not "%~1"=="--no-pause" pause
exit /b 1

:err_no_cmake
echo ERROR: CMake was not found.
echo.
echo   Install CMake 3.15 or newer, then run this script again:
echo     https://cmake.org/download/
echo   If Visual Studio 2022/2026 is installed, the CMake bundled with it is
echo   used automatically - open "Visual Studio Installer" and make sure the
echo   workload "Desktop development with C++" is installed.
echo.
if /i not "%~1"=="--no-pause" pause
exit /b 1

:err_configure_multi
echo.
echo ERROR: CMake could not configure the project.
echo.
echo   Nothing on this machine could compile C++. Please install ONE of:
echo     1. Visual Studio 2022/2026 (or Build Tools) with the workload
echo        "Desktop development with C++" (MSVC v143 + Windows SDK)
echo     2. MinGW-w64 g++ together with Ninja or mingw32-make
echo.
echo   Also install CMake 3.15+ (bundled CMake from Visual Studio works).
echo.
echo   Last CMake output:
echo   ------------------------------------------------------------
if exist "%CONFIG_LOG%" type "%CONFIG_LOG%"
echo   ------------------------------------------------------------
echo.
if /i not "%~1"=="--no-pause" pause
exit /b 1

:err_build
echo.
echo ERROR: compilation failed. The messages above show the first error.
echo.
echo   Tips:
echo     - close any program that keeps build\ locked, delete build\, retry
echo     - Visual Studio users: install "Desktop development with C++"
echo     - make sure your disk has free space and the path is writable
echo.
if /i not "%~1"=="--no-pause" pause
exit /b 1

:err_no_exe
echo ERROR: build finished but bin\Release\agentforge_web.exe is missing.
echo        Delete the build folder and run quick_build.bat again.
echo.
if /i not "%~1"=="--no-pause" pause
exit /b 1
