; BetterFxSound installer (Inno Setup 6.7; CI uses the choco "innosetup" package).
;
; Replaces the official FxSound Advanced Installer project (fxsound.aip), which
; needs a paid Advanced Installer edition. This script reproduces what that
; project does on 64-bit Windows 10/11:
;   - installs FxSound.exe, fxdiag.exe, fxmcp.exe, the built-in presets
;     (Factsoft\*.fac), the UI fonts and the driver helper tools;
;   - installs the FxSound Audio Enhancer virtual driver (fxvad) the same way the
;     DfxInstall.dll custom action does (Installer/DfxInstall/DfxInstall.cpp,
;     InstallIntelDriver), and removes it again on uninstall;
;   - creates Start menu, desktop and Startup shortcuts (working directory = the
;     install folder: the app loads presets and fonts from its working directory).
; On top of that it detects an installed official FxSound, closes it, uninstalls it
; silently (that also removes the driver, which is then installed again) and keeps
; the user's data: %APPDATA%\FxSound and HKCU\SOFTWARE\DFX are never touched.
;
; Build (paths are relative to this file unless absolute):
;   iscc /DAppVersion=1.0.1 /DFxSoundExe=<FxSound.exe> /DFxDiagExe=<fxdiag.exe>
;        [/DFxMcpExe=<fxmcp.exe>] [/DOutputDir=<dir>] Installer\BetterFxSound.iss
;
; Setup command-line switches besides the standard Inno Setup ones:
;   /SKIPDRIVER  do not install (or, for the uninstaller, remove) the virtual
;                driver. Used by the CI smoke test.

#ifndef AppVersion
  #define AppVersion "1.0.1"
#endif
#ifndef FxSoundExe
  #define FxSoundExe "..\fxsound\Project\x64\Release\App\FxSound.exe"
#endif
#ifndef FxDiagExe
  #define FxDiagExe "..\bin\x64\fxdiag.exe"
#endif
#ifndef OutputDir
  #define OutputDir "Output"
#endif

#define AppName "BetterFxSound"
#define AppPublisher "BetterFxSound"
#define AppURL "https://github.com/xp3z41x/BetterFxSound"
; New identity: not the official FxSound UpgradeCode {1CA2081B-0D5A-41DF-86E8-2788204CE340}.
#define AppGuid "{D55BD9F9-D5A8-4ADF-8A83-B49AF7CC30D9}"

[Setup]
AppId={{#AppGuid}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
LicenseFile=..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-x64-setup
SetupIconFile=Resources\fxsound.ico
UninstallDisplayIcon={app}\FxSound.exe
UninstallDisplayName={#AppName}
; x64 only. "x64os" refuses ARM64 (the x64 driver cannot run there).
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
; The app is built with the latest JUCE, which needs Windows 10 or later. The
; official installer's Windows 7 variant (JUCE 6 build, win7 driver) is not built here.
MinVersion=10.0
PrivilegesRequired=admin
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
UsedUserAreasWarning=no
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startup"; Description: "Start {#AppName} automatically when Windows starts"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#FxSoundExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#FxDiagExe}"; DestDir: "{app}"; Flags: ignoreversion
#ifdef FxMcpExe
Source: "{#FxMcpExe}"; DestDir: "{app}"; Flags: ignoreversion
#endif
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
; Built-in presets: the app reads <working dir>\Factsoft\*.fac. User presets live in
; %APPDATA%\FxSound\Presets and are not touched.
Source: "Resources\Factsoft\*.fac"; DestDir: "{app}\Factsoft"; Flags: ignoreversion
; UI fonts for the non-Latin languages, loaded from the working directory.
Source: "Resources\Fonts\*"; DestDir: "{app}"; Flags: ignoreversion
; Machine-wide default settings (fallback for %APPDATA%\FxSound\FxSound.settings),
; same as the official installer.
Source: "Resources\FxSound.settings"; DestDir: "{commonappdata}\FxSound"; Flags: ignoreversion
; Driver tools and the signed FxSound Audio Enhancer driver package (Windows 10+ x64).
Source: "Apps\Version14\DfxSetupDrv.exe"; DestDir: "{app}\Apps"; Flags: ignoreversion
Source: "Drivers\Version14\win10\x64\fxdevcon64.exe"; DestDir: "{app}\Drivers\win10\x64"; Flags: ignoreversion
Source: "Drivers\Version14\win10\x64\fxvad.inf"; DestDir: "{app}\Drivers\win10\x64"; Flags: ignoreversion
Source: "Drivers\Version14\win10\x64\fxvad.sys"; DestDir: "{app}\Drivers\win10\x64"; Flags: ignoreversion
Source: "Drivers\Version14\win10\x64\fxvadntamd64.cat"; DestDir: "{app}\Drivers\win10\x64"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\FxSound.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\FxSound.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{commonstartup}\{#AppName}"; Filename: "{app}\FxSound.exe"; WorkingDir: "{app}"; Tasks: startup

[Registry]
Root: HKLM; Subkey: "Software\{#AppName}"; Flags: uninsdeletekeyifempty
Root: HKLM; Subkey: "Software\{#AppName}\{#AppName}"; ValueType: string; ValueName: "Path"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\{#AppName}\{#AppName}"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"

[InstallDelete]
; On a reinstall or an upgrade with a task now unticked, remove the shortcut an earlier
; install created for it (Inno Setup does not remove it by itself).
Type: files; Name: "{commonstartup}\{#AppName}.lnk"; Tasks: not startup
Type: files; Name: "{autodesktop}\{#AppName}.lnk"; Tasks: not desktopicon

[Run]
Filename: "{app}\FxSound.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: dirifempty; Name: "{commonappdata}\FxSound"
Type: dirifempty; Name: "{app}"

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  FxVadHardwareId = 'root\fxvad';
  RenderKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render';
  DfxDevicesKey = 'Software\DFX\23\devices';
  DeviceStateDisabled = 2;
  WM_QUERYENDSESSION = $0011;
  ENDSESSION_CLOSEAPP = 1;
  PROCESS_QUERY_LIMITED_INFORMATION = $1000;

var
  OfficialFound: Boolean;
  OfficialRoot: Integer;
  OfficialKeyName: String;
  OfficialDisplay: String;
  RebootNeeded: Boolean;
  QuitRequests: Integer;
  AppWasRunning: Boolean;

{ Win32 imports. Types that Inno Setup 6.7 knows: handles as UINT_PTR, CreateCallback
  results as Longword. }
function ApiEnumWindows(lpEnumFunc: Longword; lParam: INT_PTR): BOOL;
  external 'EnumWindows@user32.dll stdcall';
function ApiGetWindowThreadProcessId(hWnd: HWND; var lpdwProcessId: DWORD): DWORD;
  external 'GetWindowThreadProcessId@user32.dll stdcall';
function ApiOpenProcess(dwDesiredAccess: DWORD; bInheritHandle: BOOL; dwProcessId: DWORD): UINT_PTR;
  external 'OpenProcess@kernel32.dll stdcall';
function ApiCloseHandle(hObject: UINT_PTR): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';
function ApiQueryFullProcessImageName(hProcess: UINT_PTR; dwFlags: DWORD; lpExeName: String; var lpdwSize: DWORD): BOOL;
  external 'QueryFullProcessImageNameW@kernel32.dll stdcall';

{ ---------------------------------------------------------------- helpers -- }

function HasSwitch(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

function StartsWithText(const S, Prefix: String): Boolean;
begin
  Result := CompareText(Copy(S, 1, Length(Prefix)), Prefix) = 0;
end;

{ Runs a console tool hidden and returns its stdout+stderr, trimmed. The driver
  tools report through their output ("Success", a GUID, ...), as in DfxInstall.cpp. }
function RunCapture(const Exe, Params, WorkDir: String; var Output: String): Boolean;
var
  ResultCode, I: Integer;
  ExecOut: TExecOutput;
begin
  Output := '';
  ResultCode := -1;
  Result := ExecAndCaptureOutput(Exe, Params, WorkDir, SW_HIDE, ewWaitUntilTerminated, ResultCode, ExecOut);
  if Result then
  begin
    for I := 0 to GetArrayLength(ExecOut.StdOut) - 1 do
      Output := Output + ExecOut.StdOut[I] + #13#10;
    for I := 0 to GetArrayLength(ExecOut.StdErr) - 1 do
      Output := Output + ExecOut.StdErr[I] + #13#10;
  end;
  Output := Trim(Output);
  Log(Format('Run: "%s" %s -> started=%d exit=%d output=[%s]', [Exe, Params, Ord(Result), ResultCode, Output]));
end;

function RunWait(const Exe, Params: String; var ResultCode: Integer): Boolean;
begin
  ResultCode := -1;
  Result := Exec(Exe, Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Log(Format('Run: "%s" %s -> started=%d exit=%d', [Exe, Params, Ord(Result), ResultCode]));
end;

function FxSoundRunning(): Boolean;
var
  Output: String;
begin
  { The image name is in the output only when it runs (the "no tasks" text is localized). }
  RunCapture(ExpandConstant('{sys}\tasklist.exe'), '/FI "IMAGENAME eq FxSound.exe" /FO CSV /NH',
    ExpandConstant('{sys}'), Output);
  Result := Pos('fxsound.exe', Lowercase(Output)) > 0;
end;

function IsFxSoundProcess(Pid: DWORD): Boolean;
var
  Process: UINT_PTR;
  Path: String;
  Size: DWORD;
begin
  Result := False;
  Process := ApiOpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, Pid);
  if Process = 0 then
    Exit;
  Size := 1024;
  SetLength(Path, Size);
  if ApiQueryFullProcessImageName(Process, 0, Path, Size) then
    Result := CompareText(ExtractFileName(Copy(Path, 1, Size)), 'FxSound.exe') = 0;
  ApiCloseHandle(Process);
end;

{ EnumWindows callback: asks every top-level window of an FxSound.exe process to end
  its session, as Windows does at log off. }
function PostQuitToFxSoundWindow(Wnd: HWND; Param: INT_PTR): BOOL;
var
  Pid: DWORD;
begin
  Result := True;
  Pid := 0;
  ApiGetWindowThreadProcessId(Wnd, Pid);
  if (Pid <> 0) and IsFxSoundProcess(Pid) then
    if PostMessage(Wnd, WM_QUERYENDSESSION, 0, ENDSESSION_CLOSEAPP) then
      QuitRequests := QuitRequests + 1;
end;

{ Closes FxSound.exe (official or BetterFxSound, same exe name). First the way Windows
  does at log off: JUCE answers WM_QUERYENDSESSION by quitting, so the app's shutdown
  runs and gives the Windows default device and the devices' own volume and mute back.
  WM_CLOSE (taskkill without /F) only hides its window, and a forced kill skips that
  shutdown: the FxSound virtual device stays the default with nothing processing it,
  so there is no sound until FxSound starts again. Forced only if it does not quit.
  Returns whether it was running. }
function CloseFxSound(): Boolean;
var
  ResultCode, I: Integer;
begin
  Result := FxSoundRunning();
  if not Result then
    Exit;
  QuitRequests := 0;
  ApiEnumWindows(CreateCallback(@PostQuitToFxSoundWindow), 0);
  Log(Format('Asked FxSound to quit through %d window(s)', [QuitRequests]));
  for I := 1 to 20 do
  begin
    Sleep(500);
    if not FxSoundRunning() then
    begin
      Log('FxSound closed gracefully');
      Exit;
    end;
  end;
  Log('FxSound did not quit within 10 seconds, forcing it');
  RunWait(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM FxSound.exe', ResultCode);
  if ResultCode = 0 then
    Sleep(1000);
end;

{ -------------------------------------------------- official FxSound lookup -- }

function IsOfficialEntry(const RootKey: Integer; const KeyName: String): Boolean;
var
  DisplayName, Publisher: String;
begin
  Result := False;
  if CompareText(KeyName, '{#AppGuid}_is1') = 0 then
    Exit;
  if not RegQueryStringValue(RootKey, UninstallKey + '\' + KeyName, 'DisplayName', DisplayName) then
    Exit;
  RegQueryStringValue(RootKey, UninstallKey + '\' + KeyName, 'Publisher', Publisher);
  DisplayName := Trim(DisplayName);
  { "FxSound" (Advanced Installer MSI, publisher "FxSound LLC"); also accept
    "FxSound <something>" when the publisher is FxSound. Never "BetterFxSound". }
  Result := (CompareText(DisplayName, 'FxSound') = 0) or
    (StartsWithText(DisplayName, 'FxSound ') and (Pos('fxsound', Lowercase(Publisher)) > 0));
end;

function FindOfficialIn(const RootKey: Integer): Boolean;
var
  Names: TArrayOfString;
  I: Integer;
begin
  Result := False;
  if not RegGetSubkeyNames(RootKey, UninstallKey, Names) then
    Exit;
  for I := 0 to GetArrayLength(Names) - 1 do
    if IsOfficialEntry(RootKey, Names[I]) then
    begin
      OfficialRoot := RootKey;
      OfficialKeyName := Names[I];
      RegQueryStringValue(RootKey, UninstallKey + '\' + Names[I], 'DisplayName', OfficialDisplay);
      Result := True;
      Exit;
    end;
end;

function FindOfficialFxSound(): Boolean;
begin
  Result := FindOfficialIn(HKLM64) or FindOfficialIn(HKLM32) or FindOfficialIn(HKCU);
  if Result then
    Log(Format('Official FxSound found: "%s" (uninstall key %s)', [OfficialDisplay, OfficialKeyName]))
  else
    Log('Official FxSound not found');
end;

function IsGuidText(const S: String): Boolean;
begin
  Result := (Length(S) = 38) and (S[1] = '{') and (S[38] = '}');
end;

// The product code for an MSI uninstall: the key name, or the GUID inside an
// "MsiExec.exe /X<guid>" or "/I<guid>" UninstallString.
function OfficialProductCode(): String;
var
  UninstallString: String;
  P: Integer;
begin
  Result := '';
  if IsGuidText(OfficialKeyName) then
  begin
    Result := OfficialKeyName;
    Exit;
  end;
  if RegQueryStringValue(OfficialRoot, UninstallKey + '\' + OfficialKeyName, 'UninstallString', UninstallString) and
     (Pos('msiexec', Lowercase(UninstallString)) > 0) then
  begin
    P := Pos('{', UninstallString);
    if (P > 0) and IsGuidText(Copy(UninstallString, P, 38)) then
      Result := Copy(UninstallString, P, 38);
  end;
end;

function UninstallOfficialFxSound(var ErrorMsg: String): Boolean;
var
  ProductCode, QuietCmd: String;
  ResultCode: Integer;
begin
  Result := False;
  ProductCode := OfficialProductCode();
  if ProductCode <> '' then
  begin
    { Its own uninstall runs StopFxSound, DeleteUpdateTask, the updater cleanup and
      UninstallFxSoundDriver (fxdevcon remove). It has no action that deletes
      %APPDATA%\FxSound or HKCU\SOFTWARE\DFX. }
    if not RunWait(ExpandConstant('{sys}\msiexec.exe'),
                   '/x ' + ProductCode + ' /qn /norestart REBOOT=ReallySuppress', ResultCode) then
    begin
      ErrorMsg := 'msiexec could not be started.';
      Exit;
    end;
    { 1605/1614: already gone. 3010/1641: done, restart pending. }
    if (ResultCode = 3010) or (ResultCode = 1641) then
      RebootNeeded := True
    else if (ResultCode <> 0) and (ResultCode <> 1605) and (ResultCode <> 1614) then
    begin
      ErrorMsg := Format('msiexec /x %s failed with exit code %d.', [ProductCode, ResultCode]);
      Exit;
    end;
  end
  else if RegQueryStringValue(OfficialRoot, UninstallKey + '\' + OfficialKeyName, 'QuietUninstallString', QuietCmd) and
          (Trim(QuietCmd) <> '') then
  begin
    if not RunWait(ExpandConstant('{cmd}'), '/C "' + QuietCmd + '"', ResultCode) or (ResultCode <> 0) then
    begin
      ErrorMsg := Format('The quiet uninstall command failed (exit code %d).', [ResultCode]);
      Exit;
    end;
  end
  else
  begin
    ErrorMsg := 'No silent uninstall command was found for it.';
    Exit;
  end;

  if RegKeyExists(OfficialRoot, UninstallKey + '\' + OfficialKeyName) then
  begin
    ErrorMsg := 'Its uninstall entry is still present after uninstalling.';
    Exit;
  end;
  Result := True;
end;

{ ------------------------------------------------------------------ driver -- }

{ True when a root-enumerated FxSound Audio Enhancer device node exists
  (hardware ID Root\FXVAD, created by "fxdevcon64 install"). }
function FxVadDevicePresent(): Boolean;
var
  Classes, Instances: TArrayOfString;
  I, J: Integer;
  Path, HardwareIds: String;
begin
  Result := False;
  if not RegGetSubkeyNames(HKLM64, 'SYSTEM\CurrentControlSet\Enum\ROOT', Classes) then
    Exit;
  for I := 0 to GetArrayLength(Classes) - 1 do
  begin
    Path := 'SYSTEM\CurrentControlSet\Enum\ROOT\' + Classes[I];
    if RegGetSubkeyNames(HKLM64, Path, Instances) then
      for J := 0 to GetArrayLength(Instances) - 1 do
        if RegQueryMultiStringValue(HKLM64, Path + '\' + Instances[J], 'HardwareID', HardwareIds) and
           (Pos(FxVadHardwareId, Lowercase(HardwareIds)) > 0) then
        begin
          Log('FxSound Audio Enhancer device node: ROOT\' + Classes[I] + '\' + Instances[J]);
          Result := True;
          Exit;
        end;
  end;
end;

{ Re-enables a disabled FxSound Audio Enhancer playback endpoint, like
  DfxInstall::EnableDFXDriver. Best effort: the MMDevices keys may not be
  writable for administrators on every Windows build. }
procedure EnableFxSoundEndpoints();
var
  Endpoints: TArrayOfString;
  I: Integer;
  Path, Name1, Name2, Name3: String;
  State: Cardinal;
begin
  if not RegGetSubkeyNames(HKLM64, RenderKey, Endpoints) then
    Exit;
  for I := 0 to GetArrayLength(Endpoints) - 1 do
  begin
    Path := RenderKey + '\' + Endpoints[I];
    Name1 := '';
    Name2 := '';
    Name3 := '';
    RegQueryStringValue(HKLM64, Path + '\Properties', '{a45c254e-df1c-4efd-8020-67d146a850e0},2', Name1);
    RegQueryStringValue(HKLM64, Path + '\Properties', '{b3f8fa53-0004-438e-9003-51a46e139bfc},6', Name2);
    RegQueryStringValue(HKLM64, Path + '\Properties', '{026e516e-b814-414b-83cd-856d6fef4822},2', Name3);
    if (Pos('FxSound Audio Enhancer', Name1 + '|' + Name2 + '|' + Name3) > 0) and
       RegQueryDWordValue(HKLM64, Path, 'DeviceState', State) and
       ((State and DeviceStateDisabled) <> 0) then
    begin
      if RegWriteDWordValue(HKLM64, Path, 'DeviceState', 1) then
        Log('Enabled FxSound Audio Enhancer endpoint ' + Endpoints[I])
      else
        Log('Could not enable FxSound Audio Enhancer endpoint ' + Endpoints[I]);
      Sleep(500);
    end;
  end;
end;

procedure DriverWarning(const Detail: String);
begin
  Log('Driver step failed: ' + Detail);
  SuppressibleMsgBox('The FxSound Audio Enhancer audio driver could not be installed (' + Detail + ').' + #13#10#13#10 +
    'BetterFxSound needs this driver to process audio. Restart Windows and run this setup again. ' +
    'The setup log in your %TEMP% folder has the details.', mbError, MB_OK, IDOK);
end;

{ Mirrors DfxInstall::InstallIntelDriver (x64, Windows 10 driver folder). }
procedure InstallDriver();
var
  AppsDir, DrvDir, DfxSetup, DevCon, Inf, Output, Guid: String;
  I: Integer;
begin
  AppsDir := ExpandConstant('{app}\Apps');
  DrvDir := ExpandConstant('{app}\Drivers\win10\x64');
  DfxSetup := AppsDir + '\DfxSetupDrv.exe';
  DevCon := DrvDir + '\fxdevcon64.exe';
  Inf := DrvDir + '\fxvad.inf';

  { 1. Remove the legacy DFX Audio Enhancer driver if it is still installed. }
  if RunCapture(DfxSetup, 'check', AppsDir, Output) and StartsWithText(Output, 'DFX Audio Enhancer') then
    RunCapture(DevCon, 'remove *DFX12', DrvDir, Output);

  { 2. Install the FxSound Audio Enhancer device. "fxdevcon install" always creates a
       new device node, so an existing one (left by a failed official uninstall, or a
       BetterFxSound reinstall) is reused: only its driver package is refreshed. }
  if FxVadDevicePresent() then
    RunCapture(ExpandConstant('{sys}\pnputil.exe'), '/add-driver "' + Inf + '" /install', DrvDir, Output)
  else
  begin
    if not RunCapture(DevCon, 'install "' + Inf + '"', DrvDir, Output) or (Pos('Success', Output) = 0) then
    begin
      DriverWarning('fxdevcon64 install: ' + Output);
      Exit;
    end;
    Sleep(1000);
  end;

  { 3. Wait for the playback endpoint and record its ID where the official installer does. }
  Guid := '';
  for I := 1 to 20 do
  begin
    EnableFxSoundEndpoints();
    if RunCapture(DfxSetup, 'getguid', AppsDir, Output) and (Length(Output) > 2) and
       (Output[1] = '{') and (Output[Length(Output)] = '}') then
    begin
      Guid := Output;
      Break;
    end;
    Sleep(1000);
  end;
  if Guid = '' then
  begin
    DriverWarning('the FxSound Audio Enhancer playback device did not appear');
    Exit;
  end;
  RegWriteStringValue(HKLM64, DfxDevicesKey, 'dfx_guid', Guid);

  { 4. Endpoint name, default buffer size, and keep the device from blocking sleep. }
  if not RunCapture(DfxSetup, 'setname', AppsDir, Output) or (Pos('Success', Output) = 0) then
    Log('Warning: DfxSetupDrv setname did not report Success');
  if RunCapture(DfxSetup, 'defaultbuffersize', AppsDir, Output) and (Output <> '') and (Pos('Failed', Output) = 0) then
    RegWriteStringValue(HKLM64, DfxDevicesKey, 'default_buffer_size', Output);
  RunCapture(ExpandConstant('{sys}\powercfg.exe'), '-REQUESTSOVERRIDE DRIVER "FxSound Audio Enhancer" SYSTEM', AppsDir, Output);

  Log('Driver step finished');
end;

procedure UninstallDriver();
var
  DrvDir, DevCon, Output: String;
begin
  DrvDir := ExpandConstant('{app}\Drivers\win10\x64');
  DevCon := DrvDir + '\fxdevcon64.exe';
  if FileExists(DevCon) then
    RunCapture(DevCon, 'remove', DrvDir, Output);
  { The values InstallDriver recorded describe the device that is now gone. }
  RegDeleteValue(HKLM64, DfxDevicesKey, 'dfx_guid');
  RegDeleteValue(HKLM64, DfxDevicesKey, 'default_buffer_size');
  RegDeleteKeyIfEmpty(HKLM64, DfxDevicesKey);
  RegDeleteKeyIfEmpty(HKLM64, 'Software\DFX\23');
  RegDeleteKeyIfEmpty(HKLM64, 'Software\DFX');
end;

{ The app's "Launch on system startup" toggle writes HKCU\...\Run\FxSound with the
  exe path. After official FxSound is uninstalled that value points at a file that is
  gone; drop it (the Startup shortcut task covers start-up). }
procedure RemoveStaleRunValue();
var
  Value: String;
begin
  if RegQueryStringValue(HKCU, RunKey, 'FxSound', Value) then
  begin
    Value := RemoveQuotes(Trim(Value));
    if not FileExists(Value) then
    begin
      RegDeleteValue(HKCU, RunKey, 'FxSound');
      Log('Removed stale HKCU Run value FxSound -> ' + Value);
    end;
  end;
end;

{ ------------------------------------------------------------ setup events -- }

function InitializeSetup(): Boolean;
begin
  OfficialFound := FindOfficialFxSound();
  Result := True;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := '';
  if OfficialFound then
    Result := 'Replace official FxSound:' + NewLine +
      Space + '"' + OfficialDisplay + '" will be closed and uninstalled first.' + NewLine +
      Space + 'Your FxSound settings and presets are kept.' + NewLine + NewLine;
  if MemoDirInfo <> '' then
    Result := Result + MemoDirInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + MemoTasksInfo + NewLine + NewLine;
  Result := Result + 'FxSound Audio Enhancer driver:' + NewLine + Space + 'installed or kept';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ErrorMsg: String;
begin
  Result := '';
  AppWasRunning := CloseFxSound();
  if OfficialFound then
  begin
    if not UninstallOfficialFxSound(ErrorMsg) then
    begin
      Log('Official FxSound uninstall failed: ' + ErrorMsg);
      Result := 'Setup could not uninstall the official FxSound automatically (' + ErrorMsg + ')' + #13#10#13#10 +
        'Uninstall FxSound from Settings > Apps, then run this setup again. ' +
        'Your settings and presets are kept either way.';
      Exit;
    end;
    Log('Official FxSound uninstalled; driver device present afterwards: ' + IntToStr(Ord(FxVadDevicePresent())));
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    RemoveStaleRunValue();
    if HasSwitch('/SKIPDRIVER') then
      Log('/SKIPDRIVER: driver step skipped')
    else
      InstallDriver();
    { A silent upgrade or repair has no "Launch" checkbox: start BetterFxSound again if it
      was running before, as the user, in its own folder (it loads presets and fonts from
      there). Not after replacing the official FxSound: that first start is a version
      change, which resets the engine state and shows a what's-new notice. }
    if AppWasRunning and not OfficialFound and WizardSilent() then
    begin
      if ExecAsOriginalUser(ExpandConstant('{app}\FxSound.exe'), '', ExpandConstant('{app}'),
                            SW_SHOWNORMAL, ewNoWait, ResultCode) then
        Log('Started FxSound again: it was running before setup')
      else
        Log('Could not start FxSound again: ' + SysErrorMessage(ResultCode));
    end;
  end;
end;

function NeedRestart(): Boolean;
begin
  Result := RebootNeeded;
end;

{ -------------------------------------------------------- uninstall events -- }

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Value: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    AppWasRunning := CloseFxSound();
    if HasSwitch('/SKIPDRIVER') then
      Log('/SKIPDRIVER: driver removal skipped')
    else
      UninstallDriver();
    { Remove the app's own start-up value only when it points at this install. }
    if RegQueryStringValue(HKCU, RunKey, 'FxSound', Value) and
       (Pos(Lowercase(ExpandConstant('{app}')), Lowercase(Value)) > 0) then
      RegDeleteValue(HKCU, RunKey, 'FxSound');
    { %APPDATA%\FxSound (settings, presets) and HKCU\SOFTWARE\DFX are left as they are. }
  end;
end;
