[Setup]
AppId={{8E74A2B1-5C19-4D83-9A41-2F6C8D90E199}
AppName=TJCap for iOS
AppVersion=2.0.0
AppVerName=TJCap for iOS v2.0.0
AppPublisher=TJCap Project
AppPublisherURL=https://github.com/joey971018yt/TJCap-for-iOS
DefaultDirName={autopf}\TJCap for iOS
DefaultGroupName=TJCap for iOS
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupIconFile=..\assets\app_icon.ico
UninstallDisplayIcon={app}\TJCap for iOS.exe
UninstallDisplayName=TJCap for iOS
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir=..\build-installer
OutputBaseFilename=TJCap-for-iOS-Setup-v2.0.0
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "在桌面建立「TJCap for iOS」App 捷徑 / Create Desktop Shortcut (&D)"; GroupDescription: "附加捷徑 / Additional Icons:"

[InstallDelete]
Type: files; Name: "{app}\JCast for iOS.exe"
Type: files; Name: "{autodesktop}\JCast for iOS.lnk"
Type: files; Name: "{userdesktop}\JCast for iOS.lnk"
Type: filesandordirs; Name: "{autoprograms}\JCast for iOS"
Type: filesandordirs; Name: "{userprograms}\JCast for iOS"

[Files]
Source: "..\release-msvc-win64\TJCap for iOS.exe"; DestDir: "{app}"; Flags: ignoreversion
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
Name: "{group}\TJCap for iOS"; Filename: "{app}\TJCap for iOS.exe"; IconFilename: "{app}\TJCap for iOS.exe"; Comment: "啟動 TJCap for iOS 智慧擷取控制台"
Name: "{group}\解除安裝 TJCap for iOS"; Filename: "{uninstallexe}"
Name: "{autodesktop}\TJCap for iOS"; Filename: "{app}\TJCap for iOS.exe"; IconFilename: "{app}\TJCap for iOS.exe"; Tasks: desktopicon; Comment: "啟動 TJCap for iOS 智慧擷取控制台"

[Run]
Filename: "{app}\ios_vcam_driver_setup.exe"; Parameters: "--install"; StatusMsg: "正在自動註冊 TJCap for iOS 虛擬擷取卡與 WHQL 簽章過濾驅動..."; Flags: runhidden waituntilterminated
Filename: "{app}\TJCap for iOS.exe"; Description: "立即啟動「TJCap for iOS」"; Flags: nowait postinstall skipifsilent shellexec

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/F /IM ""TJCap for iOS.exe"" /IM ""JCast for iOS.exe"" /IM ios_vcam_gui.exe /IM ios_vcam_host.exe"; Flags: runhidden; RunOnceId: "StopRunningProcesses"
Filename: "{app}\ios_vcam_driver_setup.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterDriversAndDshow"
