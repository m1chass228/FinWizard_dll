#define MyOutDir "C:\Projects\Output"
#define MySourceDir "C:\Projects\FinWizard_dll-main\build\Desktop_Qt_6_11_1_MinGW_64_bit_Release\src\gui"

#define MyAppName "FinWizard"
#define MyAppVersion "1.0"
#define MyAppExeName "FinWizardGui.exe"
#define MyAppPublisher "FinWizard Creator"

[Setup]
; Базовые настройки
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir={#MyOutDir}
OutputBaseFilename=FinWizard_Setup
Compression=lzma2
SolidCompression=yes
; Позволяет устанавливать программу даже без прав Администратора!
PrivilegesRequired=admin
; Иконка для удаления программы в панели управления
UninstallDisplayIcon={app}\{#MyAppExeName}

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 1. Главный исполняемый файл приложения
Source: "{#MySourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; 2. Только необходимые Qt-библиотеки
Source: "{#MySourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; 3. Переносим папки плагинов и ресурсов Qt
Source: "{#MySourceDir}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\translations\*"; DestDir: "{app}\translations"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\iconengines\*"; DestDir: "{app}\iconengines"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs createallsubdirs

; 4. Подтягиваем портативный Python
Source: "..\..\installer\win_python\*"; DestDir: "{app}\python"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; Ярлыки в пуске и на рабочем столе
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Запуск программы после успешной установки
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent