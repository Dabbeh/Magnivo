; Magnivo installer (Inno Setup 6).
;
; HOW TO BUILD THE INSTALLER (3 steps, run from the Magnivo folder):
;   1. Build the release exe:
;        cmake -S . -B build\rel -G Ninja -DCMAKE_PREFIX_PATH=C:\Qt\6.10.0\mingw_64 -DCMAKE_BUILD_TYPE=Release
;        cmake --build build\rel
;   2. Collect the exe + needed Qt files into deploy\ with windeployqt:
;        mkdir deploy
;        copy build\rel\Magnivo.exe deploy\
;        C:\Qt\6.10.0\mingw_64\bin\windeployqt.exe --release --no-translations deploy\Magnivo.exe
;   3. Compile this script (needs Inno Setup 6 installed):
;        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\Magnivo.iss
;      The finished installer lands in installer\Output\Magnivo-Setup-1.2.0.exe
;
; ICONS - every place Windows can show one is wired below:
;   SetupIconFile ......... icon of the installer exe itself + top of the wizard
;   UninstallDisplayIcon .. icon in Settings > Apps (uses the installed exe,
;                           which carries the icon via Magnivo.rc)
;   [Icons] entries ....... Start Menu / Desktop shortcuts take the icon from
;                           the installed Magnivo.exe (IconFilename).
;   Taskbar / window ...... Magnivo.exe sets it at runtime from :/logo.png,
;                           and Magnivo.rc bakes logo.ico into the exe file.

#define MyAppName "Magnivo"
#define MyAppVersion "1.2.0"
#define MyAppExe "Magnivo.exe"

[Setup]
AppId={{8E2B4B1A-3C7D-4E5F-9A6B-MAGNIVO00001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir=Output
OutputBaseFilename=Magnivo-Setup-{#MyAppVersion}
; Installer exe icon + wizard icon + uninstaller icon:
SetupIconFile=..\assets\logo.ico
; Icon shown in Settings > Apps > Installed apps:
UninstallDisplayIcon={app}\{#MyAppExe}
WizardStyle=modern
PrivilegesRequired=lowest
Compression=lzma2
SolidCompression=yes
VersionInfoVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} screen magnifier
VersionInfoCopyright=Magnivo

[Files]
; Everything windeployqt collected (exe + Qt DLLs + plugins).
Source: "..\deploy\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
; Start Menu shortcut (icon comes from the installed exe).
; HotKey gives a system-wide LAUNCH hotkey: Ctrl+Alt+G anywhere starts
; Magnivo - or summons its panel if already running (the app refuses a
; second instance over a local pipe, so mashing the hotkey is harmless).
; NOTE: keep the hotkey on this entry ONLY. Two shortcuts with the same
; hotkey fight over which one Windows fires.
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; IconFilename: "{app}\{#MyAppExe}"; HotKey: "ctrl+alt+g"
; Optional desktop shortcut (unchecked by default):
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; IconFilename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
