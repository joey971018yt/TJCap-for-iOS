#Requires -Version 5.1
$ErrorActionPreference = "Continue"
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
Write-Host "  iOS USB 通用虛擬擷取卡 (ios-usb-vcam) 完整移除程式" -ForegroundColor Cyan
Write-Host "=============================================================" -ForegroundColor Cyan

$runningHost = Get-Process -Name "ios_vcam_host" -ErrorAction SilentlyContinue
if ($runningHost) {
    Write-Host "[Info] 正在停止執行中的 ios_vcam_host.exe..." -ForegroundColor Yellow
    Stop-Process -Name "ios_vcam_host" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

if (Test-Path ".\ios_vcam_driver_setup.exe") {
    & ".\ios_vcam_driver_setup.exe" --uninstall
} elseif (Test-Path ".\ios_vcam_dshow.dll") {
    & regsvr32.exe /u /s ".\ios_vcam_dshow.dll"
}

Write-Host ""
Write-Host "[完成] 已移除 Apple USB UpperFilter 過濾層並反註冊 DirectShow 虛擬擷取卡。" -ForegroundColor Green
Read-Host "按 Enter 鍵關閉視窗"
