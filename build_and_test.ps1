# ============================================================================
# AgentForge build script (Windows / Visual Studio)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build_and_test.ps1
#   powershell -ExecutionPolicy Bypass -File .\build_and_test.ps1 -Run
#
# Notes:
#   - CMake locates Visual Studio (including Build Tools) by itself;
#     you do NOT need a "Developer Command Prompt".
#   - Builds all targets into <repo>\bin\Release.
# ============================================================================
param(
    [switch]$Run,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $projectRoot

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  AgentForge Build" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Project: $projectRoot"
Write-Host ""

# --- 1. toolchain check -----------------------------------------------------
Write-Host "[1/5] Checking toolchain..." -ForegroundColor Yellow

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Host "ERROR: cmake not found in PATH. Install CMake >= 3.15." -ForegroundColor Red
    exit 1
}
Write-Host "OK  CMake: $((cmake --version | Select-Object -First 1))"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $instances = & $vswhere -all -products * -format json | ConvertFrom-Json
    if ($instances.Count -gt 0) {
        foreach ($inst in $instances) {
            Write-Host ("OK  Visual Studio: {0} ({1}) -> {2}" -f `
                $inst.displayName, $inst.installationVersion, $inst.installationPath)
        }
    } else {
        Write-Host "WARN: vswhere found no Visual Studio installation." -ForegroundColor Yellow
    }
} else {
    Write-Host "WARN: vswhere not found; CMake will try its default generators." -ForegroundColor Yellow
}

if (-not (Test-Path "include/nlohmann/json.hpp")) {
    Write-Host "ERROR: include/nlohmann/json.hpp is missing." -ForegroundColor Red
    exit 1
}
Write-Host "OK  bundled headers: httplib.h / nlohmann/json.hpp"

# --- 2. build directory -----------------------------------------------------
Write-Host ""
Write-Host "[2/5] Preparing build directory..." -ForegroundColor Yellow

if ($Clean -and (Test-Path "build")) {
    Write-Host "Cleaning build/ ..."
    Remove-Item -Recurse -Force "build"
}
New-Item -ItemType Directory -Force -Path "build" | Out-Null

# --- 3. configure -----------------------------------------------------------
Write-Host ""
Write-Host "[3/5] Configuring CMake..." -ForegroundColor Yellow

if (Test-Path "build/CMakeCache.txt") {
    Write-Host "OK  Existing CMake cache found (incremental build). Use -Clean to reconfigure."
} else {
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) {
        Write-Host "INFO: VS 17 2022 generator failed, letting CMake choose..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force "build" -ErrorAction SilentlyContinue
        cmake -S . -B build
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: CMake configure failed (is a C++ workload installed?)." -ForegroundColor Red
        exit 1
    }
}

# --- 4. build ---------------------------------------------------------------
Write-Host ""
Write-Host "[4/5] Building (Release)..." -ForegroundColor Yellow
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Build failed." -ForegroundColor Red
    exit 1
}

# --- 5. verify --------------------------------------------------------------
Write-Host ""
Write-Host "[5/5] Checking outputs..." -ForegroundColor Yellow

$targets = @("agentforge.exe", "agentforge_v2.exe", "agentforge_web.exe")
$allOk = $true
foreach ($name in $targets) {
    $path = Join-Path $projectRoot "bin\Release\$name"
    if (Test-Path $path) {
        $sizeKB = [math]::Round((Get-Item $path).Length / 1KB, 1)
        Write-Host "OK  bin\Release\$name  ($sizeKB KB)"
    } else {
        Write-Host "ERROR: missing bin\Release\$name" -ForegroundColor Red
        $allOk = $false
    }
}
if (-not $allOk) { exit 1 }

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Build Complete" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Start server:"
Write-Host "  .\run_server.bat"
Write-Host "  or: .\bin\Release\agentforge_web.exe --config config.json"
Write-Host ""
Write-Host "Open: http://127.0.0.1:8080/  (setup: /setup.html)"

if ($Run) {
    Write-Host ""
    Write-Host "Starting server (Ctrl+C to stop)..." -ForegroundColor Cyan
    & ".\bin\Release\agentforge_web.exe" --config config.json
}
