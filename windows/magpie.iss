; The Windows installer, built by release-windows.yml's installer job:
;
;   ISCC /DMagpieVersion=0.1.3 /DReleaseDir=<dir of the release .exes> windows\magpie.iss
;
; One setup holds both release executables and installs the one for the
; machine: arm64 on Windows on ARM, x64 everywhere else.
;
; It installs for the current user alone, so it needs no administrator: the
; executable goes to %LOCALAPPDATA%\Programs\MAGPIE. The Start menu shortcut
; starts MAGPIE in %LOCALAPPDATA%\MAGPIE, so the data it offers to download on
; its first run lands in %LOCALAPPDATA%\MAGPIE\data, which is also where it
; looks when started from anywhere else (locate_default_data_path in
; src/impl/exec.c).

#ifndef MagpieVersion
  #error Define MagpieVersion, the MAGPIE_VERSION the executables report
#endif
#ifndef ReleaseDir
  #error Define ReleaseDir, the directory holding the release executables
#endif

[Setup]
; Identifies the installation across versions, so a newer setup upgrades it
; in place. Never change it.
AppId={{BA33CB03-B824-4EBB-A29F-173EBDB2EA50}
AppName=MAGPIE
AppVersion={#MagpieVersion}
AppPublisherURL=https://github.com/jvc56/MAGPIE
AppSupportURL=https://github.com/jvc56/MAGPIE/issues
VersionInfoVersion={#MagpieVersion}
PrivilegesRequired=lowest
DefaultDirName={autopf}\MAGPIE
DisableDirPage=yes
DisableProgramGroupPage=yes
; Windows 10 1903, as the executables need (README.md, "Windows").
MinVersion=10.0.18362
ArchitecturesAllowed=x64compatible or arm64
ArchitecturesInstallIn64BitMode=x64compatible or arm64
; For the optional PATH entry.
ChangesEnvironment=yes
SetupIconFile=magpie.ico
UninstallDisplayIcon={app}\magpie.exe
UninstallDisplayName=MAGPIE
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputBaseFilename=magpie-{#MagpieVersion}-windows-setup

[Tasks]
Name: "addtopath"; Description: "Add MAGPIE to my PATH, so that any terminal can run it as magpie"; Flags: unchecked
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "{#ReleaseDir}\magpie-{#MagpieVersion}-windows-x64.exe"; DestDir: "{app}"; DestName: "magpie.exe"; Check: not IsArm64; Flags: ignoreversion
Source: "{#ReleaseDir}\magpie-{#MagpieVersion}-windows-arm64.exe"; DestDir: "{app}"; DestName: "magpie.exe"; Check: IsArm64; Flags: ignoreversion
Source: "..\LICENSE.md"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Dirs]
; Removed on uninstall only if it is empty, so never with the data in it.
Name: "{localappdata}\MAGPIE"

[Icons]
Name: "{autoprograms}\MAGPIE"; Filename: "{app}\magpie.exe"; WorkingDir: "{localappdata}\MAGPIE"
Name: "{autodesktop}\MAGPIE"; Filename: "{app}\magpie.exe"; WorkingDir: "{localappdata}\MAGPIE"; Tasks: desktopicon

[Run]
Filename: "{app}\magpie.exe"; WorkingDir: "{localappdata}\MAGPIE"; Description: "{cm:LaunchProgram,MAGPIE}"; Flags: nowait postinstall skipifsilent

[Code]
const
  EnvironmentKey = 'Environment';

// Path, a PATH value, with every entry naming Dir taken out. Windows compares
// them case-insensitively and with or without a trailing backslash.
function PathWithout(const Path, Dir: String): String;
var
  Rest, Entry: String;
  Separator: Integer;
begin
  Result := '';
  Rest := Path;
  while Rest <> '' do
  begin
    Separator := Pos(';', Rest);
    if Separator = 0 then
    begin
      Entry := Rest;
      Rest := '';
    end
    else
    begin
      Entry := Copy(Rest, 1, Separator - 1);
      Rest := Copy(Rest, Separator + 1, Length(Rest));
    end;
    if CompareText(RemoveBackslashUnlessRoot(Entry), RemoveBackslashUnlessRoot(Dir)) <> 0 then
    begin
      if Result <> '' then
      begin
        Result := Result + ';';
      end;
      Result := Result + Entry;
    end;
  end;
end;

// Puts the install directory on the end of the user's PATH, or takes it off.
// Rerunning setup without the task takes it off too.
procedure UpdatePath(Add: Boolean);
var
  Path, Dir, Updated: String;
begin
  Dir := ExpandConstant('{app}');
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', Path) then
  begin
    Path := '';
  end;
  Updated := PathWithout(Path, Dir);
  if Add then
  begin
    if Updated <> '' then
    begin
      Updated := Updated + ';';
    end;
    Updated := Updated + Dir;
  end;
  if Updated = Path then
  begin
    Exit;
  end;
  if Updated = '' then
  begin
    RegDeleteValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path');
  end
  else
  begin
    RegWriteExpandStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', Updated);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    UpdatePath(WizardIsTaskSelected('addtopath'));
  end;
end;

// The data is several hundred MB once wordmaps are built, so uninstalling
// offers to delete it. It stays by default, and always in a silent uninstall
// with /SUPPRESSMSGBOXES.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    UpdatePath(False);
  end;
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := ExpandConstant('{localappdata}\MAGPIE\data');
    if DirExists(DataDir) and
       (SuppressibleMsgBox('Delete MAGPIE''s data as well? ' + DataDir +
          ' holds the lexica MAGPIE downloaded and the wordmaps it built ' +
          'from them. Keep it to reinstall MAGPIE without downloading them again.',
          mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES) then
    begin
      DelTree(DataDir, True, True, True);
      RemoveDir(ExpandConstant('{localappdata}\MAGPIE'));
    end;
  end;
end;
