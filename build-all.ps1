param(
    [switch]$Upload,
    [switch]$Monitor,
    [switch]$Ota,
    [string]$OtaIp = "192.168.4.1"
)

$root = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "=== 1. Build frontend ===" -ForegroundColor Cyan
Push-Location (Join-Path $root "frontend")
pnpm run build
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 1 }
Pop-Location

Write-Host "`n=== 2. Gzip compress -> C header ===" -ForegroundColor Cyan
node (Join-Path (Join-Path $root "scripts") "compress-to-c.mjs")
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Host "`n=== 3. Build firmware ===" -ForegroundColor Cyan
if ($Upload) {
    pio run --target upload
} else {
    pio run
}
if ($LASTEXITCODE -ne 0) { exit 1 }

if ($Ota) {
    Write-Host "`n=== 4. Upload OTA firmware ===" -ForegroundColor Cyan
    $otaPath = Join-Path $root ".pio\build\bw16\ota_all.bin"
    if (-not (Test-Path $otaPath)) {
        Write-Host "[ERROR] OTA file not found: $otaPath" -ForegroundColor Red
        exit 1
    }

    $uri = "http://${OtaIp}/api/ota"
    Write-Host "Uploading $otaPath to $uri ..." -ForegroundColor Yellow

    try {
        $response = Invoke-RestMethod -Uri $uri -Method Post -InFile $otaPath -ContentType "application/octet-stream" -TimeoutSec 60
        Write-Host "[SUCCESS] OTA Upload Completed!" -ForegroundColor Green
        if ($response) {
            if ($response.message) {
                Write-Host "Server response: $($response.message)" -ForegroundColor Green
            } else {
                Write-Host "Server response: $($response | ConvertTo-Json -Compress)" -ForegroundColor Green
            }
        }
    } catch {
        Write-Host "[ERROR] OTA Upload Failed: $_" -ForegroundColor Red
        exit 1
    }
}

Write-Host "`n=== DONE ===" -ForegroundColor Green

if ($Monitor) {
    pio device monitor
}
