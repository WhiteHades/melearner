#ifndef AppVersion
  #error AppVersion must be provided by package-cpp-windows.ps1
#endif
#ifndef StageDir
  #error StageDir must be provided by package-cpp-windows.ps1
#endif
#ifndef OutputDir
  #error OutputDir must be provided by package-cpp-windows.ps1
#endif
#ifndef OutputBaseName
  #error OutputBaseName must be provided by package-cpp-windows.ps1
#endif

[Setup]
AppId={{72E0FC8B-93C8-4C37-8E1D-E9643097A911}
AppName=melearner
AppVersion={#AppVersion}
AppPublisher=melearner contributors
DefaultDirName={localappdata}\Programs\melearner
DefaultGroupName=melearner
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
Uninstallable=yes
UninstallDisplayIcon={app}\melearner.exe
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile={#StageDir}\legal\LICENSE
ChangesAssociations=no
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\melearner"; Filename: "{app}\melearner.exe"
Name: "{autodesktop}\melearner"; Filename: "{app}\melearner.exe"; Tasks: desktopicon
