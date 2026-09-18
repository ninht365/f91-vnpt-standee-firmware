$ErrorActionPreference = "SilentlyContinue"
Write-Host "Dang tim cong COM cua ESP32-S2..." -ForegroundColor Cyan

$portDevice = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match "303A" -and $_.Class -match "Ports" } | Select-Object -First 1
$comPort = ""

if ($portDevice) {
    if ($portDevice.FriendlyName -match "(COM\d+)") {
        $comPort = $matches[1]
    }
}

if (-not $comPort) {
    $allPorts = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($allPorts.Count -gt 0) {
        $comPort = $allPorts[0]
    }
}

if (-not $comPort) {
    Write-Host "Khong tim thay cong COM dang ket noi!" -ForegroundColor Red
    Write-Host "👉 Hay GIU nut BOOT roi BAM NHA nut RESET de mach hien cong COM." -ForegroundColor Yellow
    exit 1
}

Write-Host "Tim thay ESP32-S2 tai cong: $comPort" -ForegroundColor Green
$binPath = Join-Path $PSScriptRoot "build\merged_flash.bin"

Write-Host "Dang nap firmware vao $comPort ..." -ForegroundColor Cyan
& {
    . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
    python -m esptool --chip esp32s2 -p $comPort --baud 460800 --before no-reset --after no-reset write-flash 0x0 "$binPath"
}

if ($LASTEXITCODE -eq 0) {
    Write-Host "`n✅ NAP FIRMWARE THANH CONG 100%!" -ForegroundColor Green
    Write-Host "👉 Hay BAM nut RESET tren mach de chay chuong trinh test man hinh." -ForegroundColor Yellow
}
