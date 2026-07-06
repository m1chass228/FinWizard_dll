#define MyAppName "FinWizard"
#define MyAppVersion "1.0"
#define MyAppExeName "FinWizardGui.exe"
#define MyAppPublisher "FinWizard Creator"

[Setup]
; Базовые настройки
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
; Куда устанавливать по умолчанию (autopf - это Program Files или AppData в зависимости от прав)
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
; Папки откуда брать и куда сохранять передаются снаружи из GitHub Actions
OutputDir={#MyOutDir}
OutputBaseFilename=FinWizard_Setup
Compression=lzma2
SolidCompression=yes
; Позволяет устанавливать программу даже без прав Администратора!
PrivilegesRequired=lowest
; Иконка для удаления программы в панели управления
UninstallDisplayIcon={app}\{#MyAppExeName}

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 1. Главный исполняемый файл приложения
Source: "{#MySourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; 2. Только необходимые Qt-библиотеки и плагины (исключаем .obj, .cpp и кэш)
Source: "{#MySourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceDir}\plugins\*"; DestDir: "{app}\plugins"; Flags: ignoreversion recursesubdirs createallsubdirs

; 3. Подтягиваем портативный Python прямо из папки репозитория
; Находясь в installer/inno/, выходим на два уровня вверх к корню проекта
Source: "..\..\installer\win_python\*"; DestDir: "{app}\python"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; Ярлыки в пуске и на рабочем столе
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Запуск программы после успешной установки
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
