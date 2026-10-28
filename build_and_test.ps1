# ============================================================================
# AgentForge build script (Windows) —— quick_build.bat 的 PowerShell 包装
#
# 真正的“找工具链 + 配置 + 编译”逻辑只有一份：quick_build.bat。
# 这里额外做产物校验，并支持 -Clean / -Run。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File .\build_and_test.ps1
#   powershell -ExecutionPolicy Bypass -File .\build_and_test.ps1 -Clean
#   powershell -ExecutionPolicy Bypass -File .\build_and_test.ps1 -Run
#
# 说明:
#   - 不需要 Developer Command Prompt；
#   - 产物输出到 <repo>\bin\Release。
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

# --- 1. 清理（可选）--------------------------------------------------------
if ($Clean -and (Test-Path "build")) {
    Write-Host "[1/3] Removing build\ ..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force "build"
}
New-Item -ItemType Directory -Force -Path "build" | Out-Null

# --- 2. 编译（复用 quick_build.bat 的工具链探测逻辑）-----------------------
Write-Host "[2/3] Building (quick_build.bat) ..." -ForegroundColor Yellow
& cmd /c "quick_build.bat --no-pause"
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: build failed. See the messages above." -ForegroundColor Red
    exit 1
}

# --- 3. 校验产物 -----------------------------------------------------------
Write-Host ""
Write-Host "[3/3] Checking outputs ..." -ForegroundColor Yellow

$targets = @("agentforge_web.exe", "agentforge.exe", "agentforge_v2.exe")
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
Write-Host "  .\run_server.bat        (starts the server and opens the browser)"
Write-Host "  or: .\bin\Release\agentforge_web.exe --config config.json"
Write-Host ""
Write-Host "First run: the setup wizard asks for API Key / endpoint / model."

if ($Run) {
    Write-Host ""
    Write-Host "Starting server (Ctrl+C to stop)..." -ForegroundColor Cyan
    & ".\bin\Release\agentforge_web.exe" --config config.json
}
