# Минимальная версия CMake для Qt6
cmake_minimum_required(VERSION 3.16)

# Название проекта
project(FinWizardGui LANGUAGES CXX)

# Включаем автоматическую обработку moc, uic, rcc
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Стандарт C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Находим Qt6
find_package(Qt6 6.5 COMPONENTS Widgets REQUIRED)

# --- 1. ОПРЕДЕЛЕНИЕ ПЛАТФОРМЕННЫХ ФАЙЛОВ (ДО add_executable) ---
set(PLATFORM_SOURCES "")

if(WIN32)
    add_compile_definitions(IS_WINDOWS)
    message(STATUS "Настройка специфичных штук для Windows...")

    # Используем CURRENT_SOURCE_DIR, чтобы путь всегда был верным относительно этого файла
    set(ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/app_icon.rc")

    if(EXISTS ${ICON_PATH})
        set(PLATFORM_SOURCES ${ICON_PATH})
    else()
        message(WARNING "Файл иконки не найден: ${ICON_PATH}")
    endif()
elseif(UNIX AND NOT APPLE)
    add_compile_definitions(IS_LINUX)
    message(STATUS "Настройка специфичных штук для Linux...")
endif()

# --- 2. ИСПОЛНЯЕМЫЙ ФАЙЛ ---
add_executable(FinWizardGui
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    resources.qrc         # Иконка для окна
    ${PLATFORM_SOURCES}
    material_icons.qrc   # Теперь переменная определена и подтянется на Windows
)

# --- 3. ЛИНКОВКА ---
target_link_libraries(FinWizardGui
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Gui
        core
        QuaZip::QuaZip
)

# --- 4. ПУТИ К ЗАГОЛОВОЧНЫМ ФАЙЛАМ ---
# Используем проектный корень (на уровень выше от src/gui)
target_include_directories(FinWizardGui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/include
)

# --- 5. КОПИРОВАНИЕ РЕСУРСОВ ---
if(EXISTS "${CMAKE_SOURCE_DIR}/plugins/examples")
    add_custom_command(TARGET FinWizardGui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:FinWizardGui>/plugins
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/plugins/examples
                $<TARGET_FILE_DIR:FinWizardGui>/plugins
        COMMENT "Копируем примеры конфигов в папку сборки"
    )
endif()

# --- 6. RPATH ДЛЯ LINUX ---
if(UNIX AND NOT APPLE)
    set_target_properties(FinWizardGui PROPERTIES
        INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()
