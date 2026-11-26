@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

echo ==============================================================
echo  Building ios-usb-vcam (Windows x64 Release - Visual Studio)
echo ==============================================================

set "ROOT_DIR=%~dp0.."
pushd "%ROOT_DIR%"

cmake -S . -B build-msvc -A x64
if errorlevel 1 (
    echo [Error] CMake configuration failed. Ensure Visual Studio C++ workload is installed.
    popd
    exit /b 1
)

cmake --build build-msvc --config Release --parallel
if errorlevel 1 (
    echo [Error] Compilation failed.
    popd
    exit /b 1
)

if not exist "dist-win64\drivers" mkdir "dist-win64\drivers"
copy /Y "build-msvc\Release\ios_vcam_dshow.dll" "dist-win64\" >nul
copy /Y "build-msvc\Release\ios_vcam_host.exe" "dist-win64\" >nul
copy /Y "build-msvc\Release\ios_vcam_driver_setup.exe" "dist-win64\" >nul
copy /Y "build-msvc\Release\ios_vcam_tests.exe" "dist-win64\" >nul
copy /Y "scripts\install_all_admin.ps1" "dist-win64\" >nul
copy /Y "scripts\uninstall_all_admin.ps1" "dist-win64\" >nul

echo [Test] Running protocol and media pipeline unit tests...
"dist-win64\ios_vcam_tests.exe"
if errorlevel 1 (
    echo [Error] Unit tests failed!
    popd
    exit /b 1
)

echo.
echo [Success] Windows x64 binaries staged in: %CD%\dist-win64
popd
exit /b 0
