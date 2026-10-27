; CursorLingo per-user installer (Inno Setup 6). No administrator rights: it installs to
; %LOCALAPPDATA%\Programs\CursorLingo, like VS Code's user installer, and shows up in Settings > Apps > Installed apps.
;
; The only registry entry is the one Windows needs for that list (HKCU\...\Uninstall\{AppId}_is1); the
; uninstaller removes it. CursorLingo itself keeps its settings in %APPDATA%\CursorLingo.
;
; Build (from the repo root, after building the exe):
;   iscc windows\CursorLingo.iss
;   iscc /DExe=path\to\CursorLingo.exe windows\CursorLingo.iss

#ifndef Exe
  #define Exe AddBackslash(SourcePath) + "..\build\windows\Release\CursorLingo.exe"
#endif
#define AppVersion GetVersionNumbersString(Exe)

[Setup]
AppId={{6C1B7E52-3F0A-4D8E-9B57-2C4E0F1A9D63}
AppName=CursorLingo
AppVersion={#AppVersion}
AppVerName=CursorLingo {#AppVersion}
AppPublisher=CursorLingo contributors
AppPublisherURL=https://github.com/kittipongint/cursor-lingo
AppSupportURL=https://github.com/kittipongint/cursor-lingo/issues
VersionInfoVersion={#AppVersion}
PrivilegesRequired=lowest
DefaultDirName={autopf}\CursorLingo
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
WizardStyle=modern
SetupIconFile=..\assets\AppIcon.ico
UninstallDisplayIcon={app}\CursorLingo.exe
UninstallDisplayName=CursorLingo
OutputDir=..\build
OutputBaseFilename=CursorLingo-Windows-x64-Setup
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Start CursorLingo with Windows"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "{#Exe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\CursorLingo"; Filename: "{app}\CursorLingo.exe"
Name: "{autodesktop}\CursorLingo"; Filename: "{app}\CursorLingo.exe"; Tasks: desktopicon
; The same shortcut the app's own "Start with Windows" menu item creates and removes.
Name: "{userstartup}\CursorLingo"; Filename: "{app}\CursorLingo.exe"; Tasks: startup

[Run]
Filename: "{app}\CursorLingo.exe"; Description: "{cm:LaunchProgram,CursorLingo}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Leave nothing behind: the Startup shortcut (if it was turned on from the tray menu later) and the settings,
; including the personal word list.
Type: files; Name: "{userstartup}\CursorLingo.lnk"
Type: filesandordirs; Name: "{userappdata}\CursorLingo"

[Code]
// CursorLingo has no main window for the Restart Manager to close, so stop it before files are replaced or removed.
procedure StopCursorLingo();
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM CursorLingo.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopCursorLingo();
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  StopCursorLingo();
  Result := True;
end;
