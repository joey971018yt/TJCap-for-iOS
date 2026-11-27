[Setup]
AppId={{8E74A2B1-5C19-4D83-9A41-2F6C8D90E199}
AppName=JCast for iOS
AppVersion=1.8.0
AppVerName=JCast for iOS v1.8.0
AppPublisher=JCast Project
AppPublisherURL=https://github.com/joey971018yt/iOS-usb-cam
DefaultDirName={autopf}\JCast for iOS
DefaultGroupName=JCast for iOS
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupIconFile=..\assets\app_icon.ico
UninstallDisplayIcon={app}\JCast for iOS.exe
UninstallDisplayName=JCast for iOS
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir=..\build-installer
OutputBaseFilename=JCast-for-iOS-Setup-v1.8.0
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "在桌面建立「JCast for iOS」App 捷徑 (&D)"; GroupDescription: "附加捷徑："

[Files]
Source: "..\release-msvc-win64\JCast for iOS.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\ios_vcam_gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\ios_vcam_host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\ios_vcam_driver_setup.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\ios_vcam_dshow.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release-msvc-win64\drivers\*"; DestDir: "{app}\drivers"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\JCast for iOS"; Filename: "{app}\JCast for iOS.exe"; IconFilename: "{app}\JCast for iOS.exe"; Comment: "啟動 JCast for iOS 智慧擷取控制台"
Name: "{group}\解除安裝 JCast for iOS"; Filename: "{uninstallexe}"
Name: "{autodesktop}\JCast for iOS"; Filename: "{app}\JCast for iOS.exe"; IconFilename: "{app}\JCast for iOS.exe"; Tasks: desktopicon; Comment: "啟動 JCast for iOS 智慧擷取控制台"

[Run]
Filename: "{app}\ios_vcam_driver_setup.exe"; Parameters: "--install"; StatusMsg: "正在自動註冊 JCast for iOS 虛擬擷取卡與 WHQL 簽章過濾驅動..."; Flags: runhidden waituntilterminated
Filename: "{app}\JCast for iOS.exe"; Description: "立即啟動「JCast for iOS」"; Flags: nowait postinstall skipifsilent shellexec

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/F /IM ""JCast for iOS.exe"" /IM ios_vcam_gui.exe /IM ios_vcam_host.exe"; Flags: runhidden; RunOnceId: "StopRunningProcesses"
Filename: "{app}\ios_vcam_driver_setup.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterDriversAndDshow"
