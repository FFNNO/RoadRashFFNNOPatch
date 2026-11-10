; Road Rash 95 FFNNO Patch - Inno Setup 6 installer.
; Build with installer\build.bat (builds ddraw.dll first) or:
;   ISCC.exe [/DAppVersion=x.y.z] installer\RoadRashFFNNOPatch.iss
; Silent install example:
;   RoadRashFFNNOPatch-Setup-0.1.0.exe /VERYSILENT /CURRENTUSER /DIR="C:\...\RoadRash"
;     /TASKS="gpuworld,save,texturepack" /PACKDIR="D:\texture-pack"
; The HD texture pack is never bundled: it is derived from the game's art.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#define AppName "Road Rash 95 FFNNO Patch"
#define AppURL "https://github.com/FFNNO/RoadRashFFNNOPatch"

[Setup]
AppId={{B94F1DE0-ED8C-4B00-9DEB-DDA083175D21}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=FFNNO
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
DefaultDirName={code:GetDefaultGameDir}
AppendDefaultDirName=no
DirExistsWarning=no
UsePreviousAppDir=yes
DisableDirPage=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
OutputDir=Output
OutputBaseFilename=RoadRashFFNNOPatch-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Messages]
english.FinishedLabelNoIcons=Setup has finished installing [name].%n%nSome antivirus tools may flag the proxy ddraw.dll because it patches the game's code in memory. It does not modify any game file.%n%nTo uninstall, use Apps & features (Settings > Apps), or delete ddraw.dll and rr95gpu.ini from the game folder.
english.FinishedLabel=Setup has finished installing [name].%n%nSome antivirus tools may flag the proxy ddraw.dll because it patches the game's code in memory. It does not modify any game file.%n%nTo uninstall, use Apps & features (Settings > Apps), or delete ddraw.dll and rr95gpu.ini from the game folder.
spanish.FinishedLabelNoIcons=El programa terminó de instalar [name].%n%nAlgunos antivirus pueden marcar el ddraw.dll proxy porque modifica el código del juego en memoria. No cambia ningún archivo del juego.%n%nPara desinstalar, usa Aplicaciones y características (Configuración > Aplicaciones) o borra ddraw.dll y rr95gpu.ini de la carpeta del juego.
spanish.FinishedLabel=El programa terminó de instalar [name].%n%nAlgunos antivirus pueden marcar el ddraw.dll proxy porque modifica el código del juego en memoria. No cambia ningún archivo del juego.%n%nPara desinstalar, usa Aplicaciones y características (Configuración > Aplicaciones) o borra ddraw.dll y rr95gpu.ini de la carpeta del juego.

[CustomMessages]
english.TaskGroup=Options (ddraw.dll and rr95gpu.ini are always installed):
spanish.TaskGroup=Opciones (ddraw.dll y rr95gpu.ini se instalan siempre):
english.TaskGpuWorld=GPU world: draw the 3D world on the GPU at 1440x1080 (Scroll Lock toggles it in race)
spanish.TaskGpuWorld=Mundo por GPU: dibuja el mundo 3D en la GPU a 1440x1080 (Bloq Despl lo alterna en carrera)
english.TaskSave=Fixed save file: "Salvar Juego" writes "Partida 1.RRS" in the game folder without dialogs and "Cargar Juego" loads it. Back up your existing .RRS saves first.
spanish.TaskSave=Guardado fijo: "Salvar Juego" escribe "Partida 1.RRS" en la carpeta del juego sin diálogos y "Cargar Juego" la carga. Haz una copia de tus partidas .RRS antes.
english.TaskTexturePack=HD texture pack: copy replacement PNGs from a folder you choose (not included in this installer)
spanish.TaskTexturePack=Pack de texturas HD: copia los PNG de reemplazo desde una carpeta que elijas (no incluido en este instalador)
english.TaskDump=Texture dump: save every new texture the game draws to texture-dump (to grow a pack)
spanish.TaskDump=Volcado de texturas: guarda cada textura nueva que dibuja el juego en texture-dump (para ampliar un pack)
english.NoGameExe=RoadRash95.exe was not found in:%n%n%1%n%nChoose the Road Rash 95 game folder (the one that contains RoadRash95.exe).
spanish.NoGameExe=No se encontró RoadRash95.exe en:%n%n%1%n%nElige la carpeta del juego Road Rash 95 (la que contiene RoadRash95.exe).
english.NotWritable=Setup cannot write to:%n%n%1%n%nRun the installer as administrator (right-click > Run as administrator) or choose another folder.
spanish.NotWritable=El instalador no puede escribir en:%n%n%1%n%nEjecuta el instalador como administrador (clic derecho > Ejecutar como administrador) o elige otra carpeta.
english.PackCaption=HD texture pack
spanish.PackCaption=Pack de texturas HD
english.PackDescription=Choose the folder that contains the texture pack PNGs.
spanish.PackDescription=Elige la carpeta que contiene los PNG del pack de texturas.
english.PackSubCaption=Files named like 64x256_0a1b2c3d.png (<width>x<height>_<crc32>.png) are copied to the texture-pack folder of the game. The pack is not included in this installer because it is derived from the game's art: build it yourself or use one you already have.
spanish.PackSubCaption=Los archivos con nombres como 64x256_0a1b2c3d.png (<ancho>x<alto>_<crc32>.png) se copian a la carpeta texture-pack del juego. El pack no viene en este instalador porque deriva del arte del juego: créalo tú o usa uno que ya tengas.
english.PackInvalid=No texture pack PNGs (named like 64x256_0a1b2c3d.png) were found in:%n%n%1
spanish.PackInvalid=No se encontraron PNG del pack de texturas (con nombres como 64x256_0a1b2c3d.png) en:%n%n%1
english.CopyingPack=Copying the HD texture pack...
spanish.CopyingPack=Copiando el pack de texturas HD...
english.PackCopyFailed=%1 texture pack file(s) could not be copied. See the setup log for details.
spanish.PackCopyFailed=No se pudieron copiar %1 archivo(s) del pack de texturas. Consulta el registro de instalación.
english.BackupFailed=Could not back up %1. Close the game and try again.
spanish.BackupFailed=No se pudo hacer copia de seguridad de %1. Cierra el juego y vuelve a intentarlo.
english.LaunchGame=Launch Road Rash 95 (Steam)
spanish.LaunchGame=Iniciar Road Rash 95 (Steam)

[Tasks]
Name: "gpuworld"; Description: "{cm:TaskGpuWorld}"; GroupDescription: "{cm:TaskGroup}"
Name: "save"; Description: "{cm:TaskSave}"; GroupDescription: "{cm:TaskGroup}"
Name: "texturepack"; Description: "{cm:TaskTexturePack}"; GroupDescription: "{cm:TaskGroup}"; Flags: unchecked
Name: "dump"; Description: "{cm:TaskDump}"; GroupDescription: "{cm:TaskGroup}"; Flags: unchecked

[Files]
Source: "..\renderer\build\Release\ddraw.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\renderer\rr95gpu.ini"; DestDir: "{app}"; Flags: ignoreversion
; The texture pack is copied by [Code] (CopyPack) from the folder the user chose, keeping only
; files named <w>x<h>_<crc32>.png; [UninstallDelete] removes texture-pack only when this
; installer created it.

[INI]
Filename: "{app}\rr95gpu.ini"; Section: "World"; Key: "GpuWorld"; String: "1"; Tasks: gpuworld
Filename: "{app}\rr95gpu.ini"; Section: "World"; Key: "GpuWorld"; String: "0"; Tasks: not gpuworld
Filename: "{app}\rr95gpu.ini"; Section: "Save"; Key: "Enabled"; String: "1"; Tasks: save
Filename: "{app}\rr95gpu.ini"; Section: "Save"; Key: "Enabled"; String: "0"; Tasks: not save
Filename: "{app}\rr95gpu.ini"; Section: "TexturePack"; Key: "Enabled"; String: "1"; Tasks: texturepack
Filename: "{app}\rr95gpu.ini"; Section: "TexturePack"; Key: "MaxMiB"; String: "1536"; Tasks: texturepack
Filename: "{app}\rr95gpu.ini"; Section: "TexturePack"; Key: "Dump"; String: "1"; Tasks: dump
Filename: "{app}\rr95gpu.ini"; Section: "TexturePack"; Key: "Dump"; String: "0"; Tasks: not dump

[UninstallDelete]
Type: files; Name: "{app}\rr95gpu.log"
Type: filesandordirs; Name: "{app}\texture-pack"; Check: PackDirCreatedByUs

[Run]
Filename: "steam://rungameid/2448570"; Description: "{cm:LaunchGame}"; \
  Flags: shellexec postinstall unchecked skipifsilent nowait

[Code]
const
  GameExe = 'RoadRash95.exe';
  BackupSuffix = '.pre-ffnno';
  PackDirName = 'texture-pack';
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{B94F1DE0-ED8C-4B00-9DEB-DDA083175D21}_is1';

var
  PackPage: TInputDirWizardPage;
  PackCreated: Boolean;   { texture-pack did not exist (or was moved to its backup) }
  PackInPlace: Boolean;   { the chosen pack folder is the game's texture-pack folder }

{ ---------- helpers ---------- }

function NormalizeDir(S: String): String;
begin
  S := Trim(S);
  StringChangeEx(S, '"', '', True);
  StringChangeEx(S, '/', '\', True);
  while (Length(S) > 3) and (S[Length(S)] = '\') do
    Delete(S, Length(S), 1);
  Result := S;
end;

function IsGameDir(Dir: String): Boolean;
begin
  Result := (Dir <> '') and FileExists(AddBackslash(NormalizeDir(Dir)) + GameExe);
end;

function IsDirWritable(Dir: String): Boolean;
var
  F: String;
begin
  F := AddBackslash(NormalizeDir(Dir)) + 'ffnno-write-test.tmp';
  Result := SaveStringToFile(F, 'x', False);
  if Result then
    DeleteFile(F);
end;

function IsDigit(C: Char): Boolean;
begin
  Result := (C >= '0') and (C <= '9');
end;

function IsLowerHex(C: Char): Boolean;
begin
  Result := IsDigit(C) or ((C >= 'a') and (C <= 'f'));
end;

{ <digits>x<digits>_<8 lowercase hex>.png }
function IsPackName(Name: String): Boolean;
var
  I, N, X: Integer;
  Prefix: String;
begin
  Result := False;
  N := Length(Name);
  if (N < 16) or (Lowercase(Copy(Name, N - 3, 4)) <> '.png') then
    Exit;
  Name := Copy(Name, 1, N - 4);
  N := Length(Name);
  if Name[N - 8] <> '_' then
    Exit;
  for I := N - 7 to N do
    if not IsLowerHex(Name[I]) then
      Exit;
  Prefix := Copy(Name, 1, N - 9);
  X := Pos('x', Prefix);
  if (X <= 1) or (X >= Length(Prefix)) then
    Exit;
  for I := 1 to Length(Prefix) do
    if (I <> X) and not IsDigit(Prefix[I]) then
      Exit;
  Result := True;
end;

function CountPackFiles(Dir: String): Integer;
var
  FR: TFindRec;
begin
  Result := 0;
  if (Dir = '') or not DirExists(Dir) then
    Exit;
  if FindFirst(AddBackslash(Dir) + '*.png', FR) then
  try
    repeat
      if ((FR.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) and IsPackName(FR.Name) then
        Result := Result + 1;
    until not FindNext(FR);
  finally
    FindClose(FR);
  end;
end;

{ ---------- game folder detection ---------- }

{ Value of a libraryfolders.vdf line such as:  "path"   "D:\\SteamLibrary" }
function VdfPathValue(Line: String): String;
var
  P: Integer;
begin
  Result := '';
  Line := Trim(Line);
  if Pos('"path"', Lowercase(Line)) <> 1 then
    Exit;
  Line := Copy(Line, 7, Length(Line));
  P := Pos('"', Line);
  if P = 0 then
    Exit;
  Line := Copy(Line, P + 1, Length(Line));
  P := Pos('"', Line);
  if P = 0 then
    Exit;
  Result := Copy(Line, 1, P - 1);
  StringChangeEx(Result, '\\', '\', True);
end;

function FindInSteam(SteamDir: String; var Found: String): Boolean;
var
  Lines: TArrayOfString;
  I: Integer;
  Lib, Cand: String;
begin
  Result := False;
  SteamDir := NormalizeDir(SteamDir);
  if SteamDir = '' then
    Exit;
  Cand := SteamDir + '\steamapps\common\RoadRash';
  if IsGameDir(Cand) then
  begin
    Found := Cand;
    Result := True;
    Exit;
  end;
  if LoadStringsFromFile(SteamDir + '\steamapps\libraryfolders.vdf', Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
    begin
      Lib := VdfPathValue(Lines[I]);
      if Lib <> '' then
      begin
        Cand := NormalizeDir(Lib) + '\steamapps\common\RoadRash';
        if IsGameDir(Cand) then
        begin
          Found := Cand;
          Result := True;
          Exit;
        end;
      end;
    end;
end;

{ 1) EA registry Path  2) Steam install + libraries  3) default Steam folder }
function GetDefaultGameDir(Param: String): String;
var
  S: String;
begin
  if RegQueryStringValue(HKLM32, 'SOFTWARE\Electronic Arts\RoadRash 95', 'Path', S) and IsGameDir(S) then
  begin
    Result := NormalizeDir(S);
    Exit;
  end;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\Valve\Steam', 'InstallPath', S) and FindInSteam(S, Result) then
    Exit;
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', S) and FindInSteam(S, Result) then
    Exit;
  Result := ExpandConstant('{commonpf32}\Steam\steamapps\common\RoadRash');
end;

{ ---------- texture pack ---------- }

function GetPackDir(Param: String): String;
begin
  Result := '';
  if PackPage <> nil then
    Result := NormalizeDir(PackPage.Values[0]);
end;

function AppPackDir: String;
begin
  Result := ExpandConstant('{app}\') + PackDirName;
end;

{ Copies every <w>x<h>_<crc32>.png from the chosen folder to the game's texture-pack. }
procedure CopyPack;
var
  Src, Dst: String;
  FR: TFindRec;
  Total, Done, Failed: Integer;
begin
  if PackInPlace then
    Exit;
  Src := AddBackslash(GetPackDir(''));
  Dst := AddBackslash(AppPackDir);
  ForceDirectories(Dst);
  Total := CountPackFiles(Src);
  Done := 0;
  Failed := 0;
  WizardForm.StatusLabel.Caption := CustomMessage('CopyingPack');
  WizardForm.ProgressGauge.Min := 0;
  WizardForm.ProgressGauge.Max := Total;
  WizardForm.ProgressGauge.Position := 0;
  if FindFirst(Src + '*.png', FR) then
  try
    repeat
      if ((FR.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) and IsPackName(FR.Name) then
      begin
        WizardForm.FilenameLabel.Caption := FR.Name;
        WizardForm.FilenameLabel.Repaint;
        if not FileCopy(Src + FR.Name, Dst + FR.Name, False) then
        begin
          Failed := Failed + 1;
          Log('Texture pack copy failed: ' + FR.Name);
        end;
        Done := Done + 1;
        WizardForm.ProgressGauge.Position := Done;
        WizardForm.ProgressGauge.Repaint;
      end;
    until not FindNext(FR);
  finally
    FindClose(FR);
  end;
  Log(Format('Texture pack: %d copied, %d failed, from %s', [Done - Failed, Failed, Src]));
  if Failed > 0 then
    SuppressibleMsgBox(FmtMessage(CustomMessage('PackCopyFailed'), [IntToStr(Failed)]),
      mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('texturepack') then
    CopyPack;
end;

function PackDirCreatedByUs: Boolean;
begin
  Result := PackCreated;
end;

{ ---------- backups ---------- }

{ True when this AppId is already installed in Dir: the files there are ours (or backed up). }
function InstalledHereBefore(Dir: String): Boolean;
var
  S: String;
begin
  Result := False;
  if not RegQueryStringValue(HKCU, UninstallKey, 'Inno Setup: App Path', S) then
    if not RegQueryStringValue(HKLM32, UninstallKey, 'Inno Setup: App Path', S) then
      if not (IsWin64 and RegQueryStringValue(HKLM64, UninstallKey, 'Inno Setup: App Path', S)) then
        Exit;
  Result := CompareText(NormalizeDir(S), NormalizeDir(Dir)) = 0;
end;

{ Copies Name to Name.pre-ffnno unless a backup already exists. }
function BackupFile(Name: String): Boolean;
var
  F, B: String;
begin
  Result := True;
  F := ExpandConstant('{app}\') + Name;
  B := F + BackupSuffix;
  if FileExists(F) and not FileExists(B) then
  begin
    Result := FileCopy(F, B, True);
    Log(Format('Backup %s -> %s: %d', [F, B, Ord(Result)]));
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  App, Pack: String;
  Ours: Boolean;
begin
  Result := '';
  App := NormalizeDir(WizardDirValue);
  if not IsGameDir(App) then
  begin
    Result := FmtMessage(CustomMessage('NoGameExe'), [App]);
    Exit;
  end;
  if not IsDirWritable(App) then
  begin
    Result := FmtMessage(CustomMessage('NotWritable'), [App]);
    Exit;
  end;

  PackCreated := False;
  PackInPlace := False;
  if WizardIsTaskSelected('texturepack') then
  begin
    Pack := GetPackDir('');
    if CountPackFiles(Pack) = 0 then
    begin
      Result := FmtMessage(CustomMessage('PackInvalid'), [Pack]);
      Exit;
    end;
    PackInPlace := CompareText(Pack, AppPackDir) = 0;
  end;

  Ours := InstalledHereBefore(App);
  if not Ours then
  begin
    if not BackupFile('ddraw.dll') then
    begin
      Result := FmtMessage(CustomMessage('BackupFailed'), ['ddraw.dll']);
      Exit;
    end;
    if not BackupFile('rr95gpu.ini') then
    begin
      Result := FmtMessage(CustomMessage('BackupFailed'), ['rr95gpu.ini']);
      Exit;
    end;
  end;

  if WizardIsTaskSelected('texturepack') and not PackInPlace then
  begin
    if not DirExists(AppPackDir) then
      PackCreated := True
    else if (not Ours) and not DirExists(AppPackDir + BackupSuffix) then
    begin
      { Move the existing pack aside; otherwise merge-overwrite into it. }
      if not RenameFile(AppPackDir, AppPackDir + BackupSuffix) then
      begin
        Result := FmtMessage(CustomMessage('BackupFailed'), [PackDirName]);
        Exit;
      end;
      Log('Moved existing texture-pack to texture-pack' + BackupSuffix);
      PackCreated := True;
    end;
  end;
end;

{ ---------- wizard ---------- }

procedure InitializeWizard;
begin
  PackPage := CreateInputDirPage(wpSelectTasks, CustomMessage('PackCaption'),
    CustomMessage('PackDescription'), CustomMessage('PackSubCaption'), False, PackDirName);
  PackPage.Add('');
  PackPage.Values[0] := ExpandConstant('{param:PACKDIR|}');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PackPage <> nil) and (PageID = PackPage.ID) and not WizardIsTaskSelected('texturepack');
end;

{ Interactive checks; PrepareToInstall repeats them so silent installs fail too. }
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if WizardSilent then
    Exit;
  if CurPageID = wpSelectDir then
  begin
    if not IsGameDir(WizardDirValue) then
    begin
      MsgBox(FmtMessage(CustomMessage('NoGameExe'), [WizardDirValue]), mbError, MB_OK);
      Result := False;
    end
    else if not IsDirWritable(WizardDirValue) then
    begin
      MsgBox(FmtMessage(CustomMessage('NotWritable'), [WizardDirValue]), mbError, MB_OK);
      Result := False;
    end;
  end
  else if (PackPage <> nil) and (CurPageID = PackPage.ID) then
  begin
    if CountPackFiles(GetPackDir('')) = 0 then
    begin
      MsgBox(FmtMessage(CustomMessage('PackInvalid'), [GetPackDir('')]), mbError, MB_OK);
      Result := False;
    end;
  end;
end;

{ ---------- uninstall ---------- }

procedure RestoreBackup(Path: String; IsDir: Boolean);
var
  B: String;
begin
  B := Path + BackupSuffix;
  if IsDir then
  begin
    if DirExists(B) and not DirExists(Path) then
      Log(Format('Restore %s: %d', [Path, Ord(RenameFile(B, Path))]));
  end
  else if FileExists(B) and not FileExists(Path) then
    Log(Format('Restore %s: %d', [Path, Ord(RenameFile(B, Path))]));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    App := ExpandConstant('{app}\');
    RestoreBackup(App + 'ddraw.dll', False);
    RestoreBackup(App + 'rr95gpu.ini', False);
    RestoreBackup(App + PackDirName, True);
  end;
end;
