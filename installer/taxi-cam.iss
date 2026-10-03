#ifndef PayloadDir
  #error PayloadDir is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef BuildNumber
  #error BuildNumber is required
#endif
#ifndef AppIcon
  #error AppIcon is required
#endif
#ifndef OutputBase
  #define OutputBase "taxi-cam-test-setup"
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
SetupIconFile={#AppIcon}
#ifdef InstallerTest
CreateUninstallRegKey=no
UsePreviousAppDir=no
#endif

[Files]
; The new companion runs Setup's checks before any installed file changes.
Source: "{#PayloadDir}\taxi-cam.exe"; DestDir: "{tmp}"; Flags: dontcopy
Source: "{#PayloadDir}\taxi-cam.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\taxi-camera-bridge.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
; Camera mounts are user calibration: kept on update and uninstall, replaced
; only by an explicit settings reset. A first installation imports the
; simulator folder's calibration; a known installation never resurrects it.
Source: "{#PayloadDir}\taxi-camera-mounts.cfg"; DestDir: "{app}"; Flags: ignoreversion uninsneveruninstall; Check: ResetMounts
Source: "{code:SimulatorMounts}"; DestDir: "{app}"; DestName: "taxi-camera-mounts.cfg"; Flags: external onlyifdoesntexist skipifsourcedoesntexist uninsneveruninstall; Check: ImportSimulatorMounts
Source: "{#PayloadDir}\taxi-camera-mounts.cfg"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall; Check: InstallBundledMounts

[InstallDelete]
Type: files; Name: "{app}\380-taxi-cam.exe"
#ifndef InstallerTest
Type: files; Name: "{userprograms}\380 Taxi Cam.lnk"

[Icons]
Name: "{userprograms}\Taxi Cam"; Filename: "{app}\taxi-cam.exe"; WorkingDir: "{app}"
#endif
; Settings are retained unless the user explicitly chooses removal in the uninstaller.
; Installation records, logs and historical backups are retained.

[Code]
var
  SimulatorPage: TInputDirWizardPage;
  StartupPage: TInputOptionWizardPage;
  XmlPage: TInputFileWizardPage;
  SettingsPage: TInputOptionWizardPage;
  KnownInstallation, RemoveSavedSettings: Boolean;
  PreviousDir, StartupNotice: String;
  DiscoveredSimulator, InheritedXml: String;
  ExplicitXml, XmlEdited, LoadingChoices: Boolean;

function Q(Value: String): String;
begin
  Result := '"' + Value + '"';
end;

function InitializeSetup: Boolean;
var
  Choice: String;
begin
  Choice := ExpandConstant('{param:STARTUP|}');
  Result := (Choice = '') or (CompareText(Choice, 'automatic') = 0) or (CompareText(Choice, 'manual') = 0);
  if not Result then begin
    Log('Invalid /STARTUP value: ' + Choice);
    SuppressibleMsgBox('The /STARTUP option must be automatic or manual.', mbError, MB_OK, IDOK);
  end;
end;

function StartupMode: String;
begin
  if StartupPage.SelectedValueIndex = 1 then Result := 'manual'
  else Result := 'automatic';
end;

function ResetSettings: Boolean;
begin
  Result := not SettingsPage.Values[0];
end;

function SimulatorMounts(Param: String): String;
begin
  Result := AddBackslash(SimulatorPage.Values[0]) + 'taxi-camera-mounts.cfg';
end;

function ResetMounts: Boolean;
begin
  Result := ResetSettings;
end;

function ImportSimulatorMounts: Boolean;
begin
  Result := not ResetSettings and not KnownInstallation and FileExists(SimulatorMounts(''));
end;

function InstallBundledMounts: Boolean;
begin
  Result := not ResetSettings and not ImportSimulatorMounts;
end;

procedure XmlChoiceChanged(Sender: TObject);
begin
  if not LoadingChoices then XmlEdited := True;
end;

procedure ClearStaleInheritedXml;
begin
  { A saved startup file belongs to the simulator discovered with it. Never
    carry that default to another simulator, including a silent override. }
  if not ExplicitXml and not XmlEdited and (InheritedXml <> '') and
    (CompareText(ExpandFileName(XmlPage.Values[0]), ExpandFileName(InheritedXml)) = 0) and
    (CompareText(AddBackslash(ExpandFileName(SimulatorPage.Values[0])),
      AddBackslash(ExpandFileName(DiscoveredSimulator))) <> 0) then begin
    LoadingChoices := True;
    try
      XmlPage.Values[0] := '';
    finally
      LoadingChoices := False;
    end;
    Log('Simulator selection changed; cleared the inherited exe.xml path.');
  end;
end;

{ Runs one of the companion's installer commands without showing its window. }
function RunSetupCommand(Executable, Command, Destination, State, Arguments: String): Boolean;
var
  ExitCode: Integer;
begin
  Result := Exec(Executable, '--setup ' + Command + ' --destination ' + Q(Destination) + ' --state ' + Q(State) + Arguments,
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  if Result then Result := ExitCode = 0;
end;

function InstallArguments: String;
begin
  Result := ' --simulator ' + Q(SimulatorPage.Values[0]) + ' --startup ' + StartupMode;
  if (StartupMode = 'automatic') and (XmlPage.Values[0] <> '') then
    Result := Result + ' --exe-xml ' + Q(XmlPage.Values[0]);
  if ResetSettings then Result := Result + ' --reset-settings';
end;

function ErrorText(State: String): String;
var
  Text: AnsiString;
begin
  Result := 'Installation failed. Close MSFS and the companion and verify the selected paths.';
  if LoadStringFromFile(State + '\error.txt', Text) then Result := UTF8Decode(Text);
end;

function DiscoverChoices: Boolean;
var
  Ini, Choice: String;
begin
  Result := True;
  if PreviousDir = WizardDirValue then exit;
  Result := RunSetupCommand(ExpandConstant('{tmp}\taxi-cam.exe'), 'discover', WizardDirValue, ExpandConstant('{tmp}\state'), '');
  if not Result then exit;
  PreviousDir := WizardDirValue;
  Ini := ExpandConstant('{tmp}\state\choices.ini');
  DiscoveredSimulator := GetIniString('Paths', 'Simulator', '', Ini);
  InheritedXml := GetIniString('Paths', 'ExeXml', '', Ini);
  LoadingChoices := True;
  try
    SimulatorPage.Values[0] := ExpandConstant('{param:SIMULATORDIR|' + DiscoveredSimulator + '}');
    XmlPage.Values[0] := ExpandConstant('{param:EXEXML|' + InheritedXml + '}');
  finally
    LoadingChoices := False;
  end;
  XmlEdited := False;
  Choice := ExpandConstant('{param:STARTUP|' + GetIniString('Paths', 'Startup', 'automatic', Ini) + '}');
  if CompareText(Choice, 'manual') = 0 then StartupPage.SelectedValueIndex := 1
  else StartupPage.SelectedValueIndex := 0;
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
  StartupPage := CreateInputOptionPage(SimulatorPage.ID, 'Startup preference', 'Choose how to start Taxi Cam',
    'Automatic startup can be retried by running setup again. If setup cannot safely configure it, Taxi Cam will still be installed for manual launch.', True, False);
  StartupPage.Add('Configure automatic startup with MSFS');
  StartupPage.Add('Launch manually; leave existing startup entries unchanged');
  StartupPage.SelectedValueIndex := 0;
  XmlPage := CreateInputFilePage(StartupPage.ID, 'Automatic startup', 'Select the simulator exe.xml',
    'Setup preserves other startup entries and adds Taxi Cam. A new exe.xml can be created at the selected path.');
  XmlPage.Add('Simulator launch configuration:', 'XML files|*.xml|All files|*.*', '.xml');
  XmlPage.Edits[0].OnChange := @XmlChoiceChanged;
  ExplicitXml := ExpandConstant('{param:EXEXML|}') <> '';
  SettingsPage := CreateInputOptionPage(XmlPage.ID, 'Saved settings',
    'Keep your settings or start with the defaults',
    'Keep your camera profiles, calibration, reference guides and keyboard shortcuts. Camera frame rate is set to 10 (minimum 5, range 5–60). Clear this box to reset saved settings to the defaults.',
    False, False);
  SettingsPage.Add('&Keep existing settings (recommended)');
  SettingsPage.Values[0] := ExpandConstant('{param:RESETSETTINGS|0}') <> '1';
  ExtractTemporaryFile('taxi-cam.exe');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and (PreviousDir <> WizardDirValue) then begin
    if not DiscoverChoices then begin
      MsgBox('Cannot read the previous installation settings. Check installation.json.', mbError, MB_OK);
      Result := False; exit;
    end;
  end;
  if CurPageID = SimulatorPage.ID then begin
    Result := FileExists(AddBackslash(SimulatorPage.Values[0]) + 'FlightSimulator2024.exe');
    if Result then ClearStaleInheritedXml
    else MsgBox('Select the Content directory containing FlightSimulator2024.exe.', mbError, MB_OK);
  end;
  if CurPageID = XmlPage.ID then begin
    { Silent setup delegates an empty choice to the companion's existing
      discovery/manual-startup fallback; interactive setup requests a path. }
    if WizardSilent and (XmlPage.Values[0] = '') then exit;
    Result := (CompareText(ExtractFileName(XmlPage.Values[0]), 'exe.xml') = 0) and (ExtractFileDrive(XmlPage.Values[0]) <> '');
    if not Result then MsgBox('Choose a full path ending in exe.xml.', mbError, MB_OK);
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = XmlPage.ID) and (StartupMode = 'manual');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Arguments: String;
begin
  Result := '';
  if not DiscoverChoices then begin
    Result := 'Cannot read the previous installation settings. Check installation.json.';
    exit;
  end;
  ClearStaleInheritedXml;
  KnownInstallation := FileExists(AddBackslash(WizardDirValue) + 'installation.json');
  { MSFS and the companion must be closed before the bridge DLL is replaced. }
  Arguments := InstallArguments + ' --update-from-pid ' + ExpandConstant('{param:UPDATEFROMPID|0}');
  if ResetSettings then Arguments := Arguments + ' --settings-closed';
  if not RunSetupCommand(ExpandConstant('{tmp}\taxi-cam.exe'), 'check', WizardDirValue, ExpandConstant('{tmp}\state'), Arguments) then begin
    Result := ErrorText(ExpandConstant('{tmp}\state'));
    Log('Taxi Cam installation check failed: ' + Result);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  State: String;
  Text: AnsiString;
begin
  if CurStep <> ssPostInstall then exit;
  { Configure automatic startup and the installation record with the installed companion. }
  State := ExpandConstant('{tmp}\state');
  if not RunSetupCommand(ExpandConstant('{app}\taxi-cam.exe'), 'configure', ExpandConstant('{app}'), State, InstallArguments) then begin
    StartupNotice := 'Taxi Cam was installed, but Setup could not finish configuring it: ' + ErrorText(State) +
      ' Run Setup again, or open Taxi Cam from the Start menu when you use MSFS.';
    Log('Taxi Cam configuration failed: ' + StartupNotice);
    SuppressibleMsgBox(StartupNotice, mbError, MB_OK, IDOK);
    exit;
  end;
  if LoadStringFromFile(State + '\warning.txt', Text) then
    StartupNotice := UTF8Decode(Text)
  else if StartupMode = 'manual' then
    StartupNotice := 'Launch Taxi Cam from the Start menu before using MSFS. Existing simulator startup entries were left unchanged. You can run setup again to configure automatic startup.';
  if StartupNotice <> '' then Log('Taxi Cam startup notice: ' + StartupNotice);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (StartupNotice <> '') then begin
    WizardForm.FinishedLabel.Caption := 'Taxi Cam was installed successfully.' + #13#10#13#10 + StartupNotice;
    WizardForm.FinishedLabel.AutoSize := False;
    WizardForm.FinishedLabel.Height := WizardForm.FinishedPage.ClientHeight - WizardForm.FinishedLabel.Top - ScaleY(16);
  end;
end;

function UninstallCompanion: String;
begin
  Result := ExpandConstant('{app}\taxi-cam.exe');
end;

function InitializeUninstall: Boolean;
var
  Choice: Integer;
  Arguments: String;
begin
  Result := True;
  RemoveSavedSettings := ExpandConstant('{param:REMOVESETTINGS|0}') = '1';
  if not FileExists(UninstallCompanion) then exit;
  if RemoveSavedSettings then Arguments := ' --settings-closed';
  Result := RunSetupCommand(UninstallCompanion, 'check', ExpandConstant('{app}'), ExpandConstant('{tmp}\taxi-uninstall'), Arguments);
  if not Result then MsgBox(ErrorText(ExpandConstant('{tmp}\taxi-uninstall')), mbError, MB_OK);
  if Result and not UninstallSilent then begin
    Choice := TaskDialogMsgBox('Keep your Taxi Cam settings?',
      'Keep your camera profiles, calibration, reference guides and keyboard shortcuts for a future installation. Logs will be kept either way.',
      mbConfirmation, MB_YESNOCANCEL, ['&Keep settings (recommended)'#13#10'Uninstall the app and keep saved settings.',
       '&Remove saved settings'#13#10'Uninstall the app and remove saved settings.', '&Cancel'], 0);
    Result := (Choice = IDYES) or (Choice = IDNO);
    RemoveSavedSettings := Choice = IDNO;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Arguments: String;
begin
  if (CurUninstallStep <> usUninstall) or not FileExists(UninstallCompanion) then exit;
  if RemoveSavedSettings then Arguments := ' --remove-settings';
  { Remove this installation's automatic startup entries and, when chosen, saved settings. }
  if not RunSetupCommand(UninstallCompanion, 'uninstall', ExpandConstant('{app}'), ExpandConstant('{tmp}\taxi-uninstall'), Arguments) then begin
    MsgBox(ErrorText(ExpandConstant('{tmp}\taxi-uninstall')), mbError, MB_OK);
    Abort;
  end;
end;
