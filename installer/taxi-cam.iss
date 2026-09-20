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
#ifndef SettingsScript
  #error SettingsScript is required
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
SetupIconFile={#AppIcon}
#ifdef InstallerTest
CreateUninstallRegKey=no
UsePreviousAppDir=no
#endif

[Files]
; Native binaries only ever enter the live directory through the guarded transaction.
Source: "{#PayloadDir}\*"; DestDir: "{tmp}\payload"; Flags: dontcopy recursesubdirs createallsubdirs ignoreversion
Source: "{#RuntimeScript}"; DestDir: "{tmp}"; DestName: "runtime.ps1"; Flags: dontcopy
Source: "{#SettingsScript}"; DestDir: "{tmp}"; DestName: "settings.ps1"; Flags: dontcopy

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
; Settings are retained unless the user explicitly chooses removal in the uninstaller.
; Installation records, logs and historical backups are retained.

[Code]
#include InternalDir + "\uninstall-scripts.iss"
var
  SimulatorPage: TInputDirWizardPage;
  StartupPage: TInputOptionWizardPage;
  XmlPage: TInputFileWizardPage;
  SettingsPage: TInputOptionWizardPage;
  Prepared, Completed: Boolean;
  RemoveSavedSettings: Boolean;
  PreviousDir, StartupNotice: String;
  DiscoveredSimulator, InheritedXml: String;
  ExplicitXml, XmlEdited, LoadingChoices: Boolean;
  InterfaceChinese: Boolean;

function Q(Value: String): String;
begin
  Result := '"' + Value + '"';
end;

function GetUserDefaultUILanguage: LongWord; external 'GetUserDefaultUILanguage@kernel32.dll stdcall';

procedure DetectInterfaceLanguage;
var
  Choice: Integer;
  LangId: Cardinal;
begin
  { Match the companion's saved language first, then the Windows UI language.
    zh-CN and zh-SG carry Simplified text; Traditional locales keep English. }
  Choice := StrToIntDef(GetIniString('companion', 'language', '',
    ExpandConstant('{localappdata}\Taxi Cam\settings.ini')), 0);
  if Choice = 2 then InterfaceChinese := True
  else if Choice = 1 then InterfaceChinese := False
  else begin
    LangId := GetUserDefaultUILanguage and $FFFF;
    InterfaceChinese := (LangId = $0804) or (LangId = $1004);
  end;
end;

function T(English, Chinese: String): String;
begin
  if InterfaceChinese then Result := Chinese else Result := English;
end;

function InitializeSetup: Boolean;
var
  Choice: String;
begin
  DetectInterfaceLanguage;
  Result := FileExists(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'));
  if not Result then
    SuppressibleMsgBox(T('Taxi Cam setup and updates require Windows PowerShell 5.1. Restore the Windows PowerShell component and run setup again.',
      'Taxi Cam 的安装与更新需要 Windows PowerShell 5.1。请恢复 Windows PowerShell 组件后重新运行安装程序。'), mbError, MB_OK, IDOK);
  if not Result then exit;
  Choice := ExpandConstant('{param:STARTUP|}');
  Result := (Choice = '') or (CompareText(Choice, 'automatic') = 0) or (CompareText(Choice, 'manual') = 0);
  if not Result then begin
    Log('Invalid /STARTUP value: ' + Choice);
    SuppressibleMsgBox(T('The /STARTUP option must be automatic or manual.',
      '/STARTUP 选项只能是 automatic 或 manual。'), mbError, MB_OK, IDOK);
  end;
end;

function StartupMode: String;
begin
  if StartupPage.SelectedValueIndex = 1 then Result := 'Manual'
  else Result := 'Automatic';
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

function RunHelper(Mode, Script, Destination, State: String): Boolean;
var
  Args: String;
  ExitCode: Integer;
begin
  Args := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' + Q(Script) +
    ' -Mode ' + Mode + ' -Destination ' + Q(Destination) + ' -StateDirectory ' + Q(State);
  if Mode = 'Install' then begin
    Args := Args + ' -PayloadDirectory ' + Q(ExpandConstant('{tmp}\payload')) +
      ' -SimulatorDirectory ' + Q(SimulatorPage.Values[0]) + ' -StartupMode ' + StartupMode +
      ' -UpdateFromPid ' + Q(ExpandConstant('{param:UPDATEFROMPID|0}'));
    if (StartupMode = 'Automatic') and (XmlPage.Values[0] <> '') then
      Args := Args + ' -ExeXml ' + Q(XmlPage.Values[0]);
    if not SettingsPage.Values[0] then Args := Args + ' -ResetSettings';
  end;
  if ((Mode = 'Uninstall') or (Mode = 'CheckClosed')) and RemoveSavedSettings then
    Args := Args + ' -RemoveSettings';
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Args, '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  if Result then Result := ExitCode = 0;
end;

function ErrorText(State: String): String;
var
  Text: AnsiString;
begin
  Result := T('Installation failed. Close MSFS and the companion and verify the selected paths.',
    '安装失败。请关闭 MSFS 与 Taxi Cam 伴随程序，并检查所选路径。');
  if LoadStringFromFile(State + '\error.txt', Text) then Result := UTF8Decode(Text);
end;

function DiscoverChoices: Boolean;
var
  Ini, Choice: String;
begin
  Result := True;
  if PreviousDir = WizardDirValue then exit;
  Result := RunHelper('Discover', ExpandConstant('{tmp}\runtime.ps1'), WizardDirValue, ExpandConstant('{tmp}\state'));
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
    T('Select the simulator Content directory', '选择模拟器的 Content 目录'),
    T('Choose the directory containing FlightSimulator2024.exe.', '请选择包含 FlightSimulator2024.exe 的目录。'), False, '');
  SimulatorPage.Add(T('Simulator Content directory:', '模拟器 Content 目录：'));
  StartupPage := CreateInputOptionPage(SimulatorPage.ID, T('Startup preference', '启动方式'),
    T('Choose how to start Taxi Cam', '选择 Taxi Cam 的启动方式'),
    T('Automatic startup can be retried by running setup again. If setup cannot safely configure it, Taxi Cam will still be installed for manual launch.',
      '自动启动可以在重新运行安装程序时重试。若安装程序无法安全完成配置，Taxi Cam 仍会正常安装，你可以手动启动。'), True, False);
  StartupPage.Add(T('Configure automatic startup with MSFS', '配置随 MSFS 自动启动'));
  StartupPage.Add(T('Launch manually; leave existing startup entries unchanged', '手动启动；不更改现有启动项'));
  StartupPage.SelectedValueIndex := 0;
  XmlPage := CreateInputFilePage(StartupPage.ID, T('Automatic startup', '自动启动'),
    T('Select the simulator exe.xml', '选择模拟器的 exe.xml'),
    T('Setup preserves other startup entries and adds Taxi Cam. A new exe.xml can be created at the selected path.',
      '安装程序会保留其他启动项并添加 Taxi Cam。也可以在所选路径新建 exe.xml。'));
  XmlPage.Add(T('Simulator launch configuration:', '模拟器启动配置文件：'),
    T('XML files|*.xml|All files|*.*', 'XML 文件|*.xml|所有文件|*.*'), '.xml');
  XmlPage.Edits[0].OnChange := @XmlChoiceChanged;
  ExplicitXml := ExpandConstant('{param:EXEXML|}') <> '';
  SettingsPage := CreateInputOptionPage(XmlPage.ID, T('Saved settings', '已保存的设置'),
    T('Keep your settings or start with the defaults', '保留你的设置，或恢复默认设置'),
    T('Keep your camera profiles, calibration, reference guides and keyboard shortcuts. Camera frame rate is set to 10 (minimum 5, range 5–60). Clear this box to reset saved settings to the defaults.',
      '保留摄像头机型配置、校准、参考标线与键盘快捷键。摄像头帧率设为 10（最低 5，范围 5–60）。取消勾选可把已保存的设置恢复为默认值。'),
    False, False);
  SettingsPage.Add(T('&Keep existing settings (recommended)', '保留现有设置（推荐）'));
  SettingsPage.Values[0] := ExpandConstant('{param:RESETSETTINGS|0}') <> '1';
  ExtractTemporaryFile('runtime.ps1');
  ExtractTemporaryFile('settings.ps1');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and (PreviousDir <> WizardDirValue) then begin
    if not DiscoverChoices then begin
      MsgBox(T('Cannot read the previous installation settings. Check installation.json.',
        '无法读取上一次的安装设置。请检查 installation.json。'), mbError, MB_OK);
      Result := False; exit;
    end;
  end;
  if CurPageID = SimulatorPage.ID then begin
    Result := FileExists(AddBackslash(SimulatorPage.Values[0]) + 'FlightSimulator2024.exe');
    if Result then ClearStaleInheritedXml
    else MsgBox(T('Select the Content directory containing FlightSimulator2024.exe.',
      '请选择包含 FlightSimulator2024.exe 的 Content 目录。'), mbError, MB_OK);
  end;
  if CurPageID = XmlPage.ID then begin
    { Silent setup delegates an empty choice to the installer's existing
      discovery/manual-startup fallback; interactive setup requests a path. }
    if WizardSilent and (XmlPage.Values[0] = '') then exit;
    Result := (CompareText(ExtractFileName(XmlPage.Values[0]), 'exe.xml') = 0) and (ExtractFileDrive(XmlPage.Values[0]) <> '');
    if not Result then MsgBox(T('Choose a full path ending in exe.xml.', '请选择以 exe.xml 结尾的完整路径。'), mbError, MB_OK);
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = XmlPage.ID) and (StartupMode = 'Manual');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Text: AnsiString;
begin
  Result := '';
  if Prepared then exit;
  if not DiscoverChoices then begin
    Result := T('Cannot read the previous installation settings. Check installation.json.',
      '无法读取上一次的安装设置。请检查 installation.json。');
    exit;
  end;
  ClearStaleInheritedXml;
  ExtractTemporaryFiles('{tmp}\payload\*');
  if not RunHelper('Install', ExpandConstant('{tmp}\runtime.ps1'), WizardDirValue, ExpandConstant('{tmp}\state')) then begin
    Result := ErrorText(ExpandConstant('{tmp}\state'));
    Log('Taxi Cam installation failed: ' + Result);
    exit;
  end;
  Prepared := True;
  if LoadStringFromFile(ExpandConstant('{tmp}\state\warning.txt'), Text) then
    StartupNotice := UTF8Decode(Text)
  else if StartupMode = 'Manual' then
    StartupNotice := T('Launch Taxi Cam from the Start menu before using MSFS. Existing simulator startup entries were left unchanged. You can run setup again to configure automatic startup.',
      '请先从开始菜单启动 Taxi Cam，再使用 MSFS。现有模拟器的启动项未作更改。你可以重新运行安装程序来配置自动启动。');
  if StartupNotice <> '' then Log('Taxi Cam startup notice: ' + StartupNotice);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (StartupNotice <> '') then begin
    WizardForm.FinishedLabel.Caption := T('Taxi Cam was installed successfully.', 'Taxi Cam 已成功安装。') + #13#10#13#10 + StartupNotice;
    WizardForm.FinishedLabel.AutoSize := False;
    WizardForm.FinishedLabel.Height := WizardForm.FinishedPage.ClientHeight - WizardForm.FinishedLabel.Top - ScaleY(16);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then Completed := True;
end;

procedure DeinitializeSetup;
begin
  if Prepared and not Completed then
    if not RunHelper('Rollback', ExpandConstant('{tmp}\runtime.ps1'), WizardDirValue, ExpandConstant('{tmp}\state')) then
      MsgBox(T('Setup could not restore the previous installation. See ', '安装程序无法恢复之前的安装。请查看 ') +
        ExpandConstant('{tmp}\state\error.txt'), mbError, MB_OK);
end;

function InitializeUninstall: Boolean;
var
  Choice: Integer;
  Choices: TArrayOfString;
begin
  DetectInterfaceLanguage;
  RemoveSavedSettings := ExpandConstant('{param:REMOVESETTINGS|0}') = '1';
  WriteUninstallScripts(ExpandConstant('{tmp}\taxi-uninstall'));
  Result := RunHelper('CheckClosed', ExpandConstant('{tmp}\taxi-uninstall\runtime.ps1'), ExpandConstant('{app}'), ExpandConstant('{tmp}\taxi-uninstall'));
  if not Result then MsgBox(ErrorText(ExpandConstant('{tmp}\taxi-uninstall')), mbError, MB_OK);
  if Result and not UninstallSilent then begin
    SetArrayLength(Choices, 3);
    Choices[0] := T('&Keep settings (recommended)'#13#10'Uninstall the app and keep saved settings.', '保留设置（推荐）'#13#10'卸载程序，但保留已保存的设置。');
    Choices[1] := T('&Remove saved settings'#13#10'Uninstall the app and remove saved settings.', '删除已保存的设置'#13#10'卸载程序，并删除已保存的设置。');
    Choices[2] := T('&Cancel', '取消');
    Choice := TaskDialogMsgBox(T('Keep your Taxi Cam settings?', '保留 Taxi Cam 的设置？'),
      T('Keep your camera profiles, calibration, reference guides and keyboard shortcuts for a future installation. Logs will be kept either way.',
        '为以后的安装保留摄像头机型配置、校准、参考标线与键盘快捷键。两种方式都会保留日志。'),
      mbConfirmation, MB_YESNOCANCEL, Choices, 0);
    Result := (Choice = IDYES) or (Choice = IDNO);
    RemoveSavedSettings := Choice = IDNO;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    if not RunHelper('Uninstall', ExpandConstant('{tmp}\taxi-uninstall\runtime.ps1'), ExpandConstant('{app}'), ExpandConstant('{tmp}\taxi-uninstall')) then begin
      MsgBox(ErrorText(ExpandConstant('{tmp}\taxi-uninstall')), mbError, MB_OK);
      Abort;
    end;
end;
