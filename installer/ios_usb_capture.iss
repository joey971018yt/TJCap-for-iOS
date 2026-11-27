[Setup]
AppId={{8E74A2B1-5C19-4D83-9A41-2F6C8D90E199}
AppName=iOS USB 擷取卡 (iOS USB Capture Card)
AppVersion=1.3.0
AppVerName=iOS USB 擷取卡 v1.3.0 (2K+ Adaptive)
AppPublisher=iOS USB Capture Project
AppPublisherURL=https://github.com/joey971018yt/iOS-usb-cam
DefaultDirName={autopf}\iOS USB Capture Card
DefaultGroupName=iOS USB 擷取卡
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupIconFile=..\assets\app_icon.ico
UninstallDisplayIcon={app}\ios_vcam_gui.exe
UninstallDisplayName=iOS USB 擷取卡 (iOS USB Capture Card)
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir=..\build-installer
OutputBaseFilename=iOS-USB-Capture-Setup-v1.3.0
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "在桌面建立「iOS USB 擷取卡」App 捷徑 (&D)"; GroupDescription: "附加捷徑："

[Files]
Source: "..\release-msvc-win64\ios_vcam_gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\ios_vcam_host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\ios_vcam_driver_setup.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\ios_vcam_dshow.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\drivers\*"; DestDir: "{app}\drivers"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\iOS USB 擷取卡"; Filename: "{app}\ios_vcam_gui.exe"; IconFilename: "{app}\ios_vcam_gui.exe"; Comment: "啟動 iOS USB 2K+ 虛擬擷取卡控制台"
Name: "{group}\解除安裝 iOS USB 擷取卡"; Filename: "{uninstallexe}"
Name: "{autodesktop}\iOS USB 擷取卡"; Filename: "{app}\ios_vcam_gui.exe"; IconFilename: "{app}\ios_vcam_gui.exe"; Tasks: desktopicon; Comment: "啟動 iOS USB 2K+ 虛擬擷取卡控制台"

[Run]
Filename: "{app}\ios_vcam_driver_setup.exe"; Parameters: "--install"; StatusMsg: "正在自動註冊 DirectShow 2K+ 虛擬擷取卡與 WHQL 簽章過濾驅動..."; Flags: runhidden waituntilterminated
Filename: "{app}\ios_vcam_gui.exe"; Description: "立即啟動「iOS USB 擷取卡」App"; Flags: nowait postinstall skipifsilent shellexec

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/F /IM ios_vcam_gui.exe /IM ios_vcam_host.exe"; Flags: runhidden; RunOnceId: "StopRunningProcesses"
Filename: "{app}\ios_vcam_driver_setup.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterDriversAndDshow"
