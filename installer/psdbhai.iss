; PSDBhai installer (Inno Setup 6). Build with: build.ps1 -Installer
; One setup.exe for x64 and ARM64. Installs for the current user without
; admin rights and without questions: License > Install > Finish.
; IT admins can install for all users with:  PSDBhai-Setup.exe /ALLUSERS

#define AppName "PSDBhai"
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{FB537C37-5FEF-444B-94AB-B4CC2A90B425}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=PSDBhai
AppComments=Shows thumbnails for Photoshop PSD and PSB files in File Explorer.
AppCopyright=Copyright (c) 2026 Muhon33. MIT License.
DefaultDirName={autopf}\PSDBhai
; Version 1.0.0 was called "PSD Thumbs" and shares this AppId; never reuse its folder.
UsePreviousAppDir=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible arm64
ArchitecturesInstallIn64BitMode=x64compatible arm64
MinVersion=10.0
; build.ps1 writes obj\license.txt: LICENSE with paragraphs unwrapped to fit the page.
LicenseFile=..\obj\license.txt
WizardSmallImageFile=..\res\wizard-small-55.bmp,..\res\wizard-small-83.bmp,..\res\wizard-small-110.bmp
WizardImageAlphaFormat=defined
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
OutputDir=..\dist
OutputBaseFilename=PSDBhai-Setup-{#AppVersion}
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\PSDBhai.exe
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=100
; Ask to close the settings app if it is open during an update or uninstall.
AppMutex=PSDBhaiRunning
; The DLL can be held by the thumbnail surrogate (dllhost.exe); let Restart Manager close it.
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription={#AppName} Setup
VersionInfoCopyright=Copyright (c) 2026 Muhon33
#ifdef Sign
SignTool=signtool
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
FinishedHeadingLabel=PSDBhai is ready
FinishedLabelNoIcons=Open any folder with PSD or PSB files and choose Medium, Large or Extra large icons to see the previews.
FinishedLabel=Open any folder with PSD or PSB files and choose Medium, Large or Extra large icons to see the previews.%n%nPSDBhai is in the Start menu.

[Files]
Source: "..\bin\arm64\PSDBhai.dll"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion
Source: "..\bin\arm64\PSDBhai.exe"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion
Source: "..\bin\x64\PSDBhai.dll"; DestDir: "{app}"; Check: not IsArm64; Flags: ignoreversion
Source: "..\bin\x64\PSDBhai.exe"; DestDir: "{app}"; Check: not IsArm64; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; Remove the files and shortcut of the earlier "PSD Thumbs" build that this replaces.
Type: filesandordirs; Name: "{autopf}\PSD Thumbs"
Type: files; Name: "{autoprograms}\PSD Thumbs Settings.lnk"

[Icons]
Name: "{autoprograms}\PSDBhai"; Filename: "{app}\PSDBhai.exe"

[Run]
Filename: "{app}\PSDBhai.exe"; Description: "Open PSDBhai settings"; Flags: postinstall nowait skipifsilent unchecked

[Code]
function RegScope(): String;
begin
  if IsAdminInstallMode then Result := 'machine' else Result := 'user';
end;

// Carries over handler backups from the "PSD Thumbs" settings key, then removes it.
procedure MigrateOldSettings();
var
  Root: Integer;
  Value: String;
begin
  if IsAdminInstallMode then Root := HKLM else Root := HKCU;
  if RegQueryStringValue(Root, 'Software\PSD Thumbs', 'PreviousHandler.psd', Value) then
    RegWriteStringValue(Root, 'Software\PSDBhai', 'PreviousHandler.psd', Value);
  if RegQueryStringValue(Root, 'Software\PSD Thumbs', 'PreviousHandler.psb', Value) then
    RegWriteStringValue(Root, 'Software\PSDBhai', 'PreviousHandler.psb', Value);
  RegDeleteKeyIncludingSubkeys(Root, 'Software\PSD Thumbs');
  RegDeleteKeyIncludingSubkeys(HKCU, 'Software\PSD Thumbs');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep = ssInstall then
    MigrateOldSettings();
  if CurStep = ssPostInstall then
  begin
    if not Exec(ExpandConstant('{app}\PSDBhai.exe'), '/register ' + RegScope(), '', SW_HIDE,
                ewWaitUntilTerminated, Code) or (Code <> 0) then
      SuppressibleMsgBox('PSDBhai could not turn on thumbnails (code ' + IntToStr(Code) +
                         '). Open PSDBhai to try again.', mbError, MB_OK, IDOK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  // Runs before files are removed: take the handler out of the registry first.
  if CurUninstallStep = usUninstall then
    Exec(ExpandConstant('{app}\PSDBhai.exe'), '/unregister ' + RegScope(), '', SW_HIDE,
         ewWaitUntilTerminated, Code);
end;
