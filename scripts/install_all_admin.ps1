#Requires -Version 5.1
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# Self-elevate to Administrator if not already elevated
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "[Info] Requesting Administrator privileges..." -ForegroundColor Cyan
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit 0
}

Set-Location -LiteralPath $PSScriptRoot

Write-Host "=============================================================" -ForegroundColor Cyan
Write-Host "  iOS USB 通用虛擬擷取卡 (ios-usb-vcam) 一鍵安裝與驅動掛載程式" -ForegroundColor Cyan
Write-Host "=============================================================" -ForegroundColor Cyan

if (-not (Test-Path ".\ios_vcam_driver_setup.exe") -or -not (Test-Path ".\ios_vcam_dshow.dll")) {
    Write-Host "[錯誤] 找不到 ios_vcam_driver_setup.exe 或 ios_vcam_dshow.dll，請確認檔案完整解壓縮於同一目錄！" -ForegroundColor Red
    Read-Host "按 Enter 鍵離開"
    exit 1
}

if (-not (Test-Path ".\libusb-1.0.dll")) {
    Write-Host "[提醒] 同目錄尚未偵測到 libusb-1.0.dll（正式 USB 擷取需要，--mock 測試模式可免）。" -ForegroundColor Yellow
}

if (-not (Test-Path ".\drivers\libusb0.sys")) {
    Write-Host "[提醒] drivers\ 目錄尚未偵測到已簽章的 libusb0.sys。" -ForegroundColor Yellow
    Write-Host "       仍會為您註冊 DirectShow 視訊＋音訊虛擬擷取卡，放入 libusb0.sys 後再執行一次即可掛載 USB UpperFilter。" -ForegroundColor Yellow
}

& ".\ios_vcam_driver_setup.exe" --install
$exitCode = $LASTEXITCODE

Write-Host ""
if ($exitCode -eq 0) {
    Write-Host "[完成] DirectShow 視訊與音訊虛擬擷取卡安裝成功！" -ForegroundColor Green
    Write-Host "  1. 無手機測試：執行 .\ios_vcam_host.exe --mock" -ForegroundColor White
    Write-Host "  2. 正式 iPhone 擷取：接上已信任的 iPhone，執行 .\ios_vcam_host.exe" -ForegroundColor White
    Write-Host "  3. 開啟 OBS / Discord / Zoom 選取 'iOS USB Capture Card (Video)' 與 '(Audio)'" -ForegroundColor White
} else {
    Write-Host "[警告] 安裝程式回傳代碼 $exitCode，請檢查上方訊息。" -ForegroundColor Yellow
}

Write-Host ""
Read-Host "按 Enter 鍵關閉視窗"
