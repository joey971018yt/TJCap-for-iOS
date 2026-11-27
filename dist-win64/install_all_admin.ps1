#Requires -Version 5.1
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "[Info] Requesting Administrator privileges..." -ForegroundColor Cyan
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit 0
}

Set-Location -LiteralPath $PSScriptRoot

Write-Host "=============================================================" -ForegroundColor Cyan
Write-Host "  JCast for iOS v1.4.0 一鍵安裝與安全驅動掛載程式" -ForegroundColor Cyan
Write-Host "=============================================================" -ForegroundColor Cyan

if (-not (Test-Path ".\ios_vcam_driver_setup.exe") -or -not (Test-Path ".\ios_vcam_dshow.dll")) {
    Write-Host "[錯誤] 找不到 ios_vcam_driver_setup.exe 或 ios_vcam_dshow.dll，請確認檔案完整解壓縮於同一目錄！" -ForegroundColor Red
    Read-Host "按 Enter 鍵離開"
    exit 1
}

& ".\ios_vcam_driver_setup.exe" --install
$exitCode = $LASTEXITCODE

Write-Host ""
if ($exitCode -eq 0) {
    Write-Host "[完成] JCast for iOS 視訊與音訊虛擬擷取卡安裝成功！" -ForegroundColor Green
    Write-Host "  1. 開啟圖形介面：雙擊 'JCast for iOS.exe'" -ForegroundColor White
    Write-Host "  2. 開啟 OBS / Discord / Zoom 選取 'JCast for iOS (Video)' 與 'JCast for iOS (Audio)'" -ForegroundColor White
} else {
    Write-Host "[警告] 安裝程式回傳代碼 $exitCode，請檢查上方訊息。" -ForegroundColor Yellow
}

Write-Host ""
Read-Host "按 Enter 鍵關閉視窗"
