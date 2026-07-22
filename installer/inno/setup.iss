
#define MyOutDir "C:\Projects\Output"
#define MySourceDir "C:\Projects\FinWizard_dll-main\build\Desktop_Qt_6_11_1_MinGW_64_bit_Release\src\gui"

#define MyAppName "FinWizard"
#define MyAppVersion "2.10"
#define MyAppExeName "FinWizardGui.exe"
#define MyAppPublisher "Igor Bespalov"

[Setup]
; Базовые настройки
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}

; {autopf} автоматически выберет:
; - "C:\Program Files (x86)\FinWizard" (если запуск от админа)
; - "C:\Users\Имя\AppData\Local\Programs\FinWizard" (если запуск без прав админа)
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir={#MyOutDir}
OutputBaseFilename=FinWizard_Setup
Compression=lzma2
SolidCompression=yes

; --- Уведомление системы об изменении ассоциаций файлов ---
ChangesAssociations=yes

; --- РЕШЕНИЕ ПРОБЛЕМЫ С ПРАВАМИ АДМИНИСТРАТОРА ---
; adminorlowest: пытается получить права админа, но если пользователь отказывается или у него их нет —
; спокойно устанавливает программу в пользовательскую папку AppData без запроса UAC.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

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

; 5. Общий SDK для python-плагинов (finwizard_sdk.py) — должен лежать ИМЕННО
; в {app}\python_sdk, потому что bootstrap.py резолвит его через
; QCoreApplication::applicationDirPath() + "/python_sdk" (см. pluginengine.cpp).
; Если плагин принес свою копию SDK в архиве (<кэш плагина>\python_sdk) — та
; версия имеет приоритет, эта используется только как fallback.
Source: "..\..\installer\python_sdk\*"; DestDir: "{app}\python_sdk"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; Ярлыки в пуске и на рабочем столе
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Регистрация расширения .fwp для текущего пользователя (HKCU)
Root: HKCU; Subkey: "Software\Classes\.fwp"; ValueType: string; ValueName: ""; ValueData: "FinWizard.Package"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\FinWizard.Package"; ValueType: string; ValueName: ""; ValueData: "FinWizard Plugin Package"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\FinWizard.Package\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKCU; Subkey: "Software\Classes\FinWizard.Package\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

[Run]
; Запуск программы после успешной установки
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent