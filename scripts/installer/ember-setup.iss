; Per-user installer for a verified Ember package folder. It only places the
; first copy: updates stay with Updater.exe, which rewrites the folder as the
; normal user, so nothing here may need elevation or own the files afterwards.
; Setup refuses a folder that already has files in it.
; Compiled by scripts/package-installer.ps1.
#ifndef PackageDir
  #error Define PackageDir, AppVersion and Redist (see scripts/package-installer.ps1)
#endif
#define AppName "SF4 Ember Netplay"
; scripts/test-installer.ps1 overrides this to exercise the runtime step.
#ifndef RedistVersion
  #define RedistVersion GetVersionNumbersString(Redist)
#endif

[Setup]
AppId={{791CAF8E-B4B5-45D5-AB5C-B4864DDB8866}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisherURL=https://github.com/Confetti3/SF4-Ember-Netplay
AppSupportURL=https://github.com/Confetti3/SF4-Ember-Netplay
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
; The uninstaller is a program file; inside the package folder preflight.cmd
; would report it as a stray one.
UninstallFilesDir={localappdata}\{#AppName} Setup
UninstallDisplayIcon={app}\Launcher.exe
LicenseFile={#PackageDir}\LICENSE
SetupIconFile=..\..\src\ui\ember.ico
OutputBaseFilename=sf4-ember-netplay-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
Source: "{#Redist}"; DestDir: "{tmp}"; DestName: "vc_redist.x86.exe"; Flags: deleteafterinstall; Check: RuntimeOutdated

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\Launcher.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\Launcher.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; The runtime installs per machine, so this is the one step that asks for
; administrator rights. Declining leaves Launcher.exe to report the old runtime.
Filename: "{tmp}\vc_redist.x86.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Microsoft Visual C++ runtime..."; Check: RuntimeOutdated
Filename: "{app}\Launcher.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
// Installs the runtime when the present msvcp140 is older than the bundled one
// (from the toolset that built the package) or reports no version. Launcher's
// RuntimeIsCurrent also accepts Wine's builtin DLL and an unreadable version;
// installing the Microsoft runtime in those cases is harmless.
function RuntimeOutdated: Boolean;
var
  Installed, Bundled: Int64;
  Major, Minor, BundledMajor, BundledMinor, Revision, Build: Word;
begin
  Result := True;
  if not GetPackedVersion(ExpandConstant('{syswow64}\msvcp140.dll'), Installed) then exit;
  if not StrToVersion('{#RedistVersion}', Bundled) then exit;
  UnpackVersionComponents(Installed, Major, Minor, Revision, Build);
  UnpackVersionComponents(Bundled, BundledMajor, BundledMinor, Revision, Build);
  Result := (Major < BundledMajor) or ((Major = BundledMajor) and (Minor < BundledMinor));
end;

function FolderHasFiles(Dir: String): Boolean;
var
  Find: TFindRec;
begin
  Result := False;
  if not FindFirst(Dir + '\*', Find) then exit;
  try
    repeat
      if (Find.Name <> '.') and (Find.Name <> '..') then begin Result := True; exit; end;
    until not FindNext(Find);
  finally
    FindClose(Find);
  end;
end;

// Writing over an existing folder would bypass Updater.exe: its lock, its
// journal and the recovery of an interrupted update. Existing copies update
// themselves; this installer only places a first copy.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if FolderHasFiles(ExpandConstant('{app}')) then
    Result := 'The folder ' + ExpandConstant('{app}') + ' already contains files.' + #13#10#13#10 +
      'To update an existing copy of {#AppName}, run Updater.exe in that folder. ' +
      'To install here anyway, uninstall the existing copy first, or choose an empty folder.';
end;

// Deletes the files the installed PackageInventory.inc names and the folders
// that leaves empty.
procedure DeleteInventoryFiles(App: String);
var
  Lines: TArrayOfString;
  I, Open, Close: Integer;
  Relative, Parent: String;
begin
  if not LoadStringsFromFile(App + '\PackageInventory.inc', Lines) then exit;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    Open := Pos('("', Lines[I]);
    Close := Pos('")', Lines[I]);
    if (Pos('SF4E_PACKAGE_', Lines[I]) <> 1) or (Open = 0) or (Close <= Open) then continue;
    Relative := Copy(Lines[I], Open + 2, Close - Open - 2);
    StringChangeEx(Relative, '\\', '\', True);
    if (Relative = '') or (Pos('..', Relative) > 0) or (Pos(':', Relative) > 0) then continue;
    DeleteFile(App + '\' + Relative);
    Parent := ExtractFileDir(App + '\' + Relative);
    while (Length(Parent) > Length(App)) and RemoveDir(Parent) do Parent := ExtractFileDir(Parent);
  end;
end;

procedure DeleteUpdaterState(App: String);
begin
  DelTree(App + '\.ember-update-backups', True, True, True);
  DeleteFile(App + '\.ember-update.lock');
  DeleteFile(App + '\.ember-update-transaction-v1.json');
end;

// Updater.exe adds and replaces files this installer never logged, so the
// uninstaller removes what the installed PackageInventory.inc names, plus the
// updater's own state. Anything else in the folder is the player's and stays.
procedure CurUninstallStepChanged(Step: TUninstallStep);
var
  App, Handler: String;
begin
  if Step <> usUninstall then exit;
  App := ExpandConstant('{app}');
  DeleteInventoryFiles(App);
  DelTree(App + '\assets\selection', True, True, True);
  DeleteUpdaterState(App);
  RemoveDir(App + '\assets');
  RemoveDir(App);
  // Launcher.exe registers the ember: room link handler itself. Another copy
  // of Ember may own it by now; remove it only while it names this folder.
  if RegQueryStringValue(HKCU, 'Software\Classes\ember\shell\open\command', '', Handler) and
     (Pos(Lowercase(App + '\'), Lowercase(Handler)) > 0) then
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\ember');
end;
