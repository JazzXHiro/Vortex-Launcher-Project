; Inno Setup script for Vortex.
;
; Build with:  scripts\package_release.ps1   (which stages the payload first)
; or directly: ISCC.exe /DPayloadDir=..\dist\payload installer\vortex.iss
;
; ---------------------------------------------------------------------------
; Why this is a per-user install
; ---------------------------------------------------------------------------
; Vortex resolves every data file against the directory holding the executable
; -- see source/app_paths.h, which documents that as a deliberate decision so
; the CLI and the launcher can never disagree about where their data lives.
;
; That makes Program Files the wrong target: the app would install fine and
; then silently fail to write preferences.json, Images/ and its database,
; because a standard user cannot write there. Installing under LocalAppData
; keeps the directory writable, and has the pleasant side effect of needing no
; administrator rights and showing no UAC prompt.
;
; It is also why the VC++ runtime is deployed app-local (the DLLs sit beside
; the exe) rather than by running the redistributable installer: that installer
; requires administrator rights, which a per-user install does not have.

#ifndef PayloadDir
  #define PayloadDir "..\dist\payload"
#endif

#define AppName        "Vortex"
#define AppVersion     "1.0.0"
#define AppPublisher   "Vortex"
#define AppExeName     "VortexLauncher.exe"

[Setup]
; Keep this GUID stable forever -- it is how Windows recognises an upgrade of
; an existing install rather than a second, parallel one.
AppId={{8F3A5C21-7D4E-4B96-9E15-2C7A0D6B8E43}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\Vortex
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=no
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}

; No administrator rights, no UAC prompt. See the header comment.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; The payload is dominated by the bundled Python (numpy, pandas, scikit-learn),
; which compresses well but slowly. LZMA2/max is worth the packaging time to
; keep the download reasonable.
Compression=lzma2/max
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=VortexSetup
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The whole staged payload: the launcher, the Qt runtime, the app-local VC++
; runtime, the bundled Python interpreter and the analytics scripts.
;
; The staging script guarantees this tree contains no .env, no database and no
; personal data -- it refuses to produce a payload otherwise.
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
{ ------------------------------------------------------------------------- }
{ Uninstall: the data Vortex generated is NOT ours to delete silently.       }
{                                                                           }
{ Playtime history in particular cannot be reconstructed from anything else, }
{ so ask before removing it. Inno only removes files it installed, so        }
{ without this these would simply be orphaned in place.                     }
{ ------------------------------------------------------------------------- }

procedure DeleteUserData();
var
  AppDir: String;
begin
  AppDir := ExpandConstant('{app}');

  { Artwork and candidate art -- re-downloadable, but only with an API key. }
  DelTree(AppDir + '\Images', True, True, True);
  DelTree(AppDir + '\CandidateImages', True, True, True);

  { The analytics database and anything derived from it. }
  DeleteFile(AppDir + '\analytics\vortex.sqlite3');
  DeleteFile(AppDir + '\analytics\vortex.sqlite3-wal');
  DeleteFile(AppDir + '\analytics\vortex.sqlite3-shm');
  DeleteFile(AppDir + '\analytics\recommendations.json');
  DeleteFile(AppDir + '\analytics\recommendations_meta.json');
  DeleteFile(AppDir + '\analytics\.igdb_token.json');
  DelTree(AppDir + '\analytics\model\*.pkl', False, True, False);

  { Credentials. }
  DeleteFile(AppDir + '\analytics\.env');

  { Library state and history. }
  DeleteFile(AppDir + '\playtime_sessions.log');
  DeleteFile(AppDir + '\playtime_stats.txt');
  DeleteFile(AppDir + '\preferences.json');
  DeleteFile(AppDir + '\settings.json');
  DeleteFile(AppDir + '\wishlist.json');
  DeleteFile(AppDir + '\favorite_snapshots.json');
  DeleteFile(AppDir + '\installed_games.txt');
  DeleteFile(AppDir + '\local_game_dirs.txt');
  DeleteFile(AppDir + '\igdb_cache.txt');
  DeleteFile(AppDir + '\exe_cache.txt');
  DeleteFile(AppDir + '\game_metadata.txt');
end;

{ Generated bytecode, not user data -- Python rewrites it on every run. This is
  removed either way, so declining the prompt below does not leave __pycache__
  trees orphaned in the install folder. }
procedure DeleteGeneratedBytecode();
var
  AppDir: String;
begin
  AppDir := ExpandConstant('{app}');
  DelTree(AppDir + '\analytics\__pycache__', True, True, True);
  DelTree(AppDir + '\analytics\baselines\__pycache__', True, True, True);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    DeleteGeneratedBytecode();

    { Default is No: playtime history cannot be reconstructed from anything
      else, so an unattended or mis-clicked uninstall must not destroy it. }
    if MsgBox('Also delete your Vortex data?' + #13#10 + #13#10 +
              'This removes downloaded artwork, your playtime history, ' +
              'game preferences, wishlist and saved API keys.' + #13#10 + #13#10 +
              'Choose No to keep them for a future reinstall.',
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DeleteUserData();
  end;
end;
