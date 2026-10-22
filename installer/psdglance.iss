; PSDGlance installer (Inno Setup 6). Build with: build.ps1 -Installer
; One setup.exe for x64 and ARM64. Installs for the current user without
; admin rights and without questions: License > Install > Finish.
; IT admins can install for all users with:  PSDGlance-Setup.exe /ALLUSERS

#define AppName "PSDGlance"
#ifndef AppVersion
  #error Build with build.ps1 -Installer, which passes AppVersion from the VERSION file.
#endif

[Setup]
AppId={{FB537C37-5FEF-444B-94AB-B4CC2A90B425}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=PSDGlance
AppComments=Shows thumbnails for Photoshop PSD and PSB files in File Explorer.
AppCopyright=Copyright (c) 2026 Muhon33. MIT License.
DefaultDirName={autopf}\PSDGlance
; Earlier builds were called "PSD Thumbs" and "PSDBhai" and share this AppId;
; never reuse their folders.
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
OutputBaseFilename=PSDGlance-Setup-{#AppVersion}
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\PSDGlance.exe
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=100
; Ask to close the settings app if it is open during an update or uninstall.
AppMutex=PSDGlanceRunning
; The DLL can be held by the thumbnail surrogate (dllhost.exe); let Restart Manager close it.
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}.0
VersionInfoTextVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}.0
VersionInfoProductTextVersion={#AppVersion}
VersionInfoCompany=PSDGlance
VersionInfoDescription={#AppName} Setup
VersionInfoCopyright=Copyright (c) 2026 Muhon33
#ifdef Sign
SignTool=signtool
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
FinishedHeadingLabel=PSDGlance is ready
FinishedLabelNoIcons=Open any folder with PSD or PSB files and choose Medium, Large or Extra large icons to see the previews.
FinishedLabel=Open any folder with PSD or PSB files and choose Medium, Large or Extra large icons to see the previews.%n%nPSDGlance is in the Start menu.

[Files]
Source: "..\bin\arm64\PSDGlance.dll"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion
Source: "..\bin\arm64\PSDGlance.exe"; DestDir: "{app}"; Check: IsArm64; Flags: ignoreversion
Source: "..\bin\x64\PSDGlance.dll"; DestDir: "{app}"; Check: not IsArm64; Flags: ignoreversion
Source: "..\bin\x64\PSDGlance.exe"; DestDir: "{app}"; Check: not IsArm64; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; Remove the files and shortcuts of the earlier "PSD Thumbs" and "PSDBhai" builds this replaces.
Type: filesandordirs; Name: "{autopf}\PSD Thumbs"
Type: files; Name: "{autoprograms}\PSD Thumbs Settings.lnk"
Type: filesandordirs; Name: "{autopf}\PSDBhai"
Type: files; Name: "{autoprograms}\PSDBhai.lnk"

[Icons]
Name: "{autoprograms}\PSDGlance"; Filename: "{app}\PSDGlance.exe"

[Run]
Filename: "{app}\PSDGlance.exe"; Description: "Open PSDGlance settings"; Flags: postinstall nowait skipifsilent unchecked

[Code]
function RegScope(): String;
begin
  if IsAdminInstallMode then Result := 'machine' else Result := 'user';
end;

// Carries over settings from an earlier name's key (handler backups and the
// user's quality choice), then removes that key.
procedure MigrateKey(OldKey: String);
var
  Root: Integer;
  Value: String;
  Quality: Cardinal;
begin
  if IsAdminInstallMode then Root := HKLM else Root := HKCU;
  if RegQueryStringValue(Root, OldKey, 'PreviousHandler.psd', Value) then
    RegWriteStringValue(Root, 'Software\PSDGlance', 'PreviousHandler.psd', Value);
  if RegQueryStringValue(Root, OldKey, 'PreviousHandler.psb', Value) then
    RegWriteStringValue(Root, 'Software\PSDGlance', 'PreviousHandler.psb', Value);
  if RegQueryDWordValue(HKCU, OldKey, 'Quality', Quality) then
    RegWriteDWordValue(HKCU, 'Software\PSDGlance', 'Quality', Quality);
  RegDeleteKeyIncludingSubkeys(Root, OldKey);
  RegDeleteKeyIncludingSubkeys(HKCU, OldKey);
end;

procedure MigrateOldSettings();
begin
  MigrateKey('Software\PSD Thumbs');
  MigrateKey('Software\PSDBhai');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep = ssInstall then
    MigrateOldSettings();
  if CurStep = ssPostInstall then
  begin
    if not Exec(ExpandConstant('{app}\PSDGlance.exe'), '/register ' + RegScope(), '', SW_HIDE,
                ewWaitUntilTerminated, Code) or (Code <> 0) then
      SuppressibleMsgBox('PSDGlance could not turn on thumbnails (code ' + IntToStr(Code) +
                         '). Open PSDGlance to try again.', mbError, MB_OK, IDOK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  // Runs before files are removed: take the handler out of the registry first.
  if CurUninstallStep = usUninstall then
    Exec(ExpandConstant('{app}\PSDGlance.exe'), '/unregister ' + RegScope(), '', SW_HIDE,
         ewWaitUntilTerminated, Code);
end;
