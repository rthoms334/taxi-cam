#ifndef PayloadDir
  #error PayloadDir is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef BuildNumber
  #error BuildNumber is required
#endif
#ifndef OutputBase
  #define OutputBase "taxi-cam-test-setup"
#endif
#ifndef RuntimeScript
  #define RuntimeScript "runtime.ps1"
#endif

[Setup]
; Keep the product ID and setup mutex stable across the application rename.
#ifdef InstallerTest
AppId=380TaxiCamIsolatedInstallerTest
#else
AppId={{C8581992-5605-46CA-AF6F-60E44477C023}
#endif
AppName=Taxi Cam
AppVersion={#AppVersion}
AppVerName=Taxi Cam {#AppVersion}
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Taxi Cam\app
DefaultGroupName=Taxi Cam
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableDirPage=no
DisableProgramGroupPage=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
SetupMutex=380TaxiCamInstaller
OutputBaseFilename={#OutputBase}
Compression=lzma2
SolidCompression=yes
UninstallDisplayIcon={app}\taxi-cam.exe
#ifdef InstallerTest
CreateUninstallRegKey=no
UsePreviousAppDir=no
#endif

[Files]
; Native binaries only ever enter the live directory through the guarded transaction.
Source: "{#PayloadDir}\*"; DestDir: "{tmp}\payload"; Flags: dontcopy recursesubdirs createallsubdirs ignoreversion
Source: "{#RuntimeScript}"; DestDir: "{tmp}"; DestName: "runtime.ps1"; Flags: dontcopy

#ifndef InstallerTest
[InstallDelete]
Type: files; Name: "{userprograms}\380 Taxi Cam.lnk"

[Icons]
Name: "{userprograms}\Taxi Cam"; Filename: "{app}\taxi-cam.exe"; WorkingDir: "{app}"
#endif

[UninstallDelete]
Type: files; Name: "{app}\taxi-cam.exe"
Type: files; Name: "{app}\taxi-camera-bridge.dll"
Type: files; Name: "{app}\LICENSE.txt"
Type: files; Name: "{app}\THIRD_PARTY_NOTICES.txt"
; Calibration, installation record, user settings, logs and historical backups are retained.

[Code]
#include InternalDir + "\uninstall-scripts.iss"
var
  SimulatorPage: TInputDirWizardPage;
  XmlPage: TInputFileWizardPage;
  Prepared, Completed: Boolean;
  PreviousDir: String;

function Q(Value: String): String;
begin
  Result := '"' + Value + '"';
end;

function InitializeSetup: Boolean;
begin
  Result := FileExists(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'));
  if not Result then
    SuppressibleMsgBox('Taxi Cam setup and updates require Windows PowerShell 5.1. Restore the Windows PowerShell component and run setup again.', mbError, MB_OK, IDOK);
end;

function RunHelper(Mode, Script, Destination, State: String): Boolean;
var
  Args: String;
  ExitCode: Integer;
begin
  Args := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' + Q(Script) +
    ' -Mode ' + Mode + ' -Destination ' + Q(Destination) + ' -StateDirectory ' + Q(State);
  if Mode = 'Install' then
    Args := Args + ' -PayloadDirectory ' + Q(ExpandConstant('{tmp}\payload')) +
      ' -SimulatorDirectory ' + Q(SimulatorPage.Values[0]) + ' -ExeXml ' + Q(XmlPage.Values[0]) +
      ' -UpdateFromPid ' + Q(ExpandConstant('{param:UPDATEFROMPID|0}'));
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Args, '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  if Result then Result := ExitCode = 0;
end;

function ErrorText(State: String): String;
var
  Text: AnsiString;
begin
  Result := 'Installation failed. Close MSFS and the companion and verify the selected paths.';
  if LoadStringFromFile(State + '\error.txt', Text) then Result := UTF8Decode(Text);
end;

procedure InitializeWizard;
begin
  { Registered installs retain their directory through the stable AppId.
    Also discover the former default directory used by script installations. }
  if (CompareText(WizardDirValue, ExpandConstant('{localappdata}\Taxi Cam\app')) = 0) and
    (ExpandConstant('{param:DIR|}') = '') and
    not FileExists(AddBackslash(WizardDirValue) + 'installation.json') and
    FileExists(ExpandConstant('{localappdata}\380 Taxi Cam\app\installation.json')) then
    WizardForm.DirEdit.Text := ExpandConstant('{localappdata}\380 Taxi Cam\app');
  SimulatorPage := CreateInputDirPage(wpSelectDir, 'Microsoft Flight Simulator 2024',
    'Select the simulator Content directory', 'Choose the directory containing FlightSimulator2024.exe.', False, '');
  SimulatorPage.Add('Simulator Content directory:');
  XmlPage := CreateInputFilePage(SimulatorPage.ID, 'Automatic startup', 'Select the simulator exe.xml',
    'Setup preserves other startup entries and adds Taxi Cam. A new exe.xml can be created at the selected path.');
  XmlPage.Add('Simulator launch configuration:', 'XML files|*.xml|All files|*.*', '.xml');
  ExtractTemporaryFile('runtime.ps1');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Ini: String;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and (PreviousDir <> WizardDirValue) then begin
    if not RunHelper('Discover', ExpandConstant('{tmp}\runtime.ps1'), WizardDirValue, ExpandConstant('{tmp}\state')) then begin
      MsgBox('Cannot read the previous installation settings. Check installation.json.', mbError, MB_OK);
      Result := False; exit;
    end;
    PreviousDir := WizardDirValue;
    Ini := ExpandConstant('{tmp}\state\choices.ini');
    SimulatorPage.Values[0] := ExpandConstant('{param:SIMULATORDIR|' + GetIniString('Paths', 'Simulator', '', Ini) + '}');
    XmlPage.Values[0] := ExpandConstant('{param:EXEXML|' + GetIniString('Paths', 'ExeXml', '', Ini) + '}');
  end;
  if CurPageID = SimulatorPage.ID then begin
    Result := FileExists(AddBackslash(SimulatorPage.Values[0]) + 'FlightSimulator2024.exe');
    if not Result then MsgBox('Select the Content directory containing FlightSimulator2024.exe.', mbError, MB_OK);
  end;
  if CurPageID = XmlPage.ID then begin
    Result := (CompareText(ExtractFileName(XmlPage.Values[0]), 'exe.xml') = 0) and (ExtractFileDrive(XmlPage.Values[0]) <> '');
    if not Result then MsgBox('Choose a full path ending in exe.xml.', mbError, MB_OK);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if Prepared then exit;
  ExtractTemporaryFiles('{tmp}\payload\*');
  if not RunHelper('Install', ExpandConstant('{tmp}\runtime.ps1'), WizardDirValue, ExpandConstant('{tmp}\state')) then begin
    Result := ErrorText(ExpandConstant('{tmp}\state'));
    exit;
  end;
  Prepared := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then Completed := True;
end;

procedure DeinitializeSetup;
begin
  if Prepared and not Completed then
    if not RunHelper('Rollback', ExpandConstant('{tmp}\runtime.ps1'), WizardDirValue, ExpandConstant('{tmp}\state')) then
      MsgBox('Setup could not restore the previous installation. See ' + ExpandConstant('{tmp}\state\error.txt'), mbError, MB_OK);
end;

function InitializeUninstall: Boolean;
begin
  WriteUninstallScripts(ExpandConstant('{tmp}\taxi-uninstall'));
  Result := RunHelper('CheckClosed', ExpandConstant('{tmp}\taxi-uninstall\runtime.ps1'), ExpandConstant('{app}'), ExpandConstant('{tmp}\taxi-uninstall'));
  if not Result then MsgBox(ErrorText(ExpandConstant('{tmp}\taxi-uninstall')), mbError, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    if not RunHelper('Uninstall', ExpandConstant('{tmp}\taxi-uninstall\runtime.ps1'), ExpandConstant('{app}'), ExpandConstant('{tmp}\taxi-uninstall')) then begin
      MsgBox(ErrorText(ExpandConstant('{tmp}\taxi-uninstall')), mbError, MB_OK);
      Abort;
    end;
end;
