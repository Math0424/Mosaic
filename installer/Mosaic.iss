; ============================================================================
; Mosaic - Inno Setup script
;
; Built by the CMake `installer` target, which passes:
;   /DMyAppVersion=<version>
;   /DPackageDir=<path to the staged package directory>
;
; Requires Inno Setup 6 (https://jrsoftware.org/isdl.php).
; ============================================================================

#define MyAppName "Mosaic"
#ifndef MyAppVersion
#define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "Mosaic"
#define MyAppExeName "Mosaic.exe"
#define MyAppIcon "..\assets\icon.ico"
; PackageDir and OutputDir are supplied by the CMake `installer` target via /D.

[Setup]
AppId={{1ADB38B2-8A83-4FA9-B61C-67021BB872D7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=Mosaic-Setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile={#MyAppIcon}
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Code]
// The library folder is user-configurable and stored in state.json, so it can't
// be a static [UninstallDelete] entry — resolve it at uninstall time.

// StringReplace isn't available in PascalScript.
function ReplaceAll(const S, Old, New: string): string;
var
  I: Integer;
begin
  Result := S;
  if Old = '' then
    exit;
  I := Pos(Old, Result);
  while I > 0 do
  begin
    Result := Copy(Result, 1, I - 1) + New +
      Copy(Result, I + Length(Old), Length(Result));
    I := Pos(Old, Result);
  end;
end;

function GetLibraryPath(): string;
var
  StateFile, Json: string;
  Raw: AnsiString;
  P, KeyLen: Integer;
begin
  Result := ExpandConstant('{userappdata}\Math\Mosaic\library');
  StateFile := ExpandConstant('{userappdata}\Math\Mosaic\state.json');
  if not FileExists(StateFile) then
    exit;
  if not LoadStringFromFile(StateFile, Raw) then
    exit;
  Json := Raw;
  KeyLen := 14;  // Length('"libraryPath"')
  P := Pos('"libraryPath"', Json);
  if P = 0 then
    exit;
  Json := Copy(Json, P + KeyLen, Length(Json));
  P := Pos(':', Json);
  if P = 0 then
    exit;
  Json := Copy(Json, P + 1, Length(Json));
  Json := Trim(Json);
  if Copy(Json, 1, 1) <> '"' then
    exit;
  Delete(Json, 1, 1);
  P := Pos('"', Json);
  if P = 0 then
    exit;
  Json := Copy(Json, 1, P - 1);
  Json := ReplaceAll(Json, '\\', '\');
  Json := ReplaceAll(Json, '/', '\');
  if Json <> '' then
    Result := Json;
end;

procedure DeletePathRecursive(const Path: string);
var
  FindRec: TFindRec;
  Sub: string;
begin
  if FindFirst(Path + '\*', FindRec) then
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        Sub := Path + '\' + FindRec.Name;
        if FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0 then
          DeletePathRecursive(Sub)
        else
          DeleteFile(Sub);
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
  RemoveDir(Path);
end;

// Delete everything under Root except the subtree KeepPath.
procedure DeleteTreeContents(const Root, KeepPath: string);
var
  FindRec: TFindRec;
  Item: string;
begin
  if FindFirst(Root + '\*', FindRec) then
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        Item := Root + '\' + FindRec.Name;
        if Pos(Lowercase(KeepPath + '\'), Lowercase(Item + '\')) = 1 then
          continue;
        if FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0 then
          DeletePathRecursive(Item)
        else
          DeleteFile(Item);
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir, LibraryPath, Msg: string;
  DeleteLibrary: Boolean;
begin
  if CurUninstallStep <> usUninstall then
    exit;
  DataDir := ExpandConstant('{userappdata}\Math\Mosaic');
  LibraryPath := GetLibraryPath();

  Msg := 'Delete the Mosaic library folder (images, videos, database)?' + #13#10 +
    LibraryPath;
  DeleteLibrary := (MsgBox(Msg, mbConfirmation, MB_YESNO) = IDYES);
  if DeleteLibrary then
    DeletePathRecursive(LibraryPath);

  Msg := 'Delete user data (settings, cache, state.json)?' + #13#10 + DataDir;
  if MsgBox(Msg, mbConfirmation, MB_YESNO) = IDYES then
  begin
    // When the library lives inside the data dir and is being kept, remove
    // everything around it instead of the whole tree.
    if (not DeleteLibrary) and
       (Pos(Lowercase(DataDir + '\'), Lowercase(LibraryPath)) = 1) then
    begin
      DeleteTreeContents(DataDir, LibraryPath);
      DeleteFile(ExpandConstant('{uninstallexe}'));
    end
    else
      DeletePathRecursive(DataDir);
  end;
end;

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
