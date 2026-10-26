# ============================================================================
# AgentForge smoke test
#
# 启动一个测试实例 (config.test.json, 端口 18080), 检查:
#   - /api/health
#   - 静态页面 / 与 /setup.html
#   - /api/tools
#   - /api/config (脱敏)
# 然后自动停止实例。
#
# 注意: 不会调用外部 LLM; 端到端对话测试见 tools/mock_llm_server.py。
# ============================================================================
param(
    [string]$Config = "config.test.json",
    [int]$Port = 18080
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $projectRoot

$exe = Join-Path $projectRoot "bin\Release\agentforge_web.exe"
if (-not (Test-Path $exe)) {
    Write-Host "ERROR: $exe not found. Build first." -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $Config)) {
    Write-Host "ERROR: config not found: $Config" -ForegroundColor Red
    exit 1
}

Write-Host "[smoke] starting $exe (config=$Config, port=$Port)"
$proc = Start-Process -FilePath $exe -ArgumentList @("--config", $Config, "--port", "$Port") `
    -WorkingDirectory $projectRoot -PassThru -WindowStyle Hidden

function Wait-Endpoint([string]$url, [int]$timeoutSec = 15) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $r = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 3
            if ($r.StatusCode -ge 200 -and $r.StatusCode -lt 300) { return $r }
        } catch {
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

$failed = $false
try {
    $health = Wait-Endpoint "http://127.0.0.1:$Port/api/health" 20
    if (-not $health) {
        Write-Host "FAIL  /api/health (server did not start)" -ForegroundColor Red
        $failed = $true
    } else {
        $data = $health.Content | ConvertFrom-Json
        Write-Host "OK    /api/health  status=$($data.status) version=$($data.version) tools=$($data.tools.Count)"
    }

    foreach ($item in @(
        @{ Path = "/"; Name = "index.html" },
        @{ Path = "/setup.html"; Name = "setup.html" },
        @{ Path = "/api/tools"; Name = "tools(json)" },
        @{ Path = "/api/config"; Name = "config(json)" },
        @{ Path = "/api/permissions"; Name = "permissions(json)" },
        @{ Path = "/api/sessions"; Name = "sessions(json)" },
        @{ Path = "/api/memory"; Name = "memory(json)" },
        @{ Path = "/api/metrics"; Name = "metrics(json)" },
        @{ Path = "/api/repo-map"; Name = "repo-map(json)" }
    )) {
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port$($item.Path)" -UseBasicParsing -TimeoutSec 10
            $len = $r.Content.Length
            $minLen = if ($item.Name -in @('sessions(json)', 'memory(json)')) { 2 } else { 50 }
            if ($r.StatusCode -eq 200 -and $len -ge $minLen) {
                Write-Host "OK    $($item.Path)  ($($item.Name), $len bytes)"
            } else {
                Write-Host "FAIL  $($item.Path)  status=$($r.StatusCode) len=$len" -ForegroundColor Red
                $failed = $true
            }
        } catch {
            Write-Host "FAIL  $($item.Path)  $($_.Exception.Message)" -ForegroundColor Red
            $failed = $true
        }
    }

    try {
        $cfg = (Invoke-WebRequest -Uri "http://127.0.0.1:$Port/api/config" -UseBasicParsing).Content | ConvertFrom-Json
        if ($cfg.api.hasKey -eq $true -and $cfg.api.key -notmatch "^sk-[A-Za-z0-9]{20,}$") {
            Write-Host "OK    api key is masked"
        } elseif ($cfg.api.hasKey -eq $false) {
            Write-Host "OK    api key not configured (masking n/a)"
        } else {
            Write-Host "FAIL  api key is not masked!" -ForegroundColor Red
            $failed = $true
        }
    } catch {
        Write-Host "FAIL  config masking check: $($_.Exception.Message)" -ForegroundColor Red
        $failed = $true
    }
} finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
    }
}

if ($failed) {
    Write-Host ""
    Write-Host "SMOKE TEST FAILED" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "SMOKE TEST PASSED" -ForegroundColor Green
exit 0
