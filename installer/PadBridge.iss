#define AppName "PadBridge"
#define AppVersion "1.0.0"
#define AppPublisher "PadBridge"
#define SourceDir GetEnv("PADBRIDGE_DIST")

[Setup]
AppId={{8F0E6347-86CD-4D90-A27A-37C82D7F3904}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\PadBridge
DefaultGroupName=PadBridge
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\release
OutputBaseFilename=PadBridge-Setup-{#AppVersion}
SetupIconFile=..\windows-controller\Assets\PadBridge.ico
UninstallDisplayIcon={app}\PadBridge.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
AppMutex=Local\PadBridge.Controller
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription=Windows to iPad extended display
LicenseFile=..\LICENSE

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\PadBridge"; Filename: "{app}\PadBridge.exe"
Name: "{userdesktop}\PadBridge"; Filename: "{app}\PadBridge.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "PadBridge"; ValueData: """{app}\PadBridge.exe"" --background"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\PadBridge.exe"; Description: "Launch PadBridge"; Flags: nowait postinstall skipifsilent
