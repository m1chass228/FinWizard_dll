cmake_minimum_required(VERSION 3.20)
project(FinWizard VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Чтобы на Windows не открывалась консоль вместе с GUI
set(CMAKE_WIN32_EXECUTABLE ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 1. Находим Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)

# 2. Настройка QuaZip
set(QUAZIP_QT_MAJOR_VERSION 6 CACHE STRING "" FORCE)
set(QUAZIP_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

if(WIN32)
    # На Windows через vcpkg часто лучше системный ZLIB
    set(QUAZIP_USE_QT_ZLIB OFF CACHE BOOL "" FORCE)
else()
    # На Linux ОБЯЗАТЕЛЬНО выключаем использование ZLIB из Qt,
    # так как его там нет — используем системный
    set(QUAZIP_USE_QT_ZLIB OFF CACHE BOOL "" FORCE)
endif()

# Собираем QuaZip статически, чтобы не мучиться с путями .so/.dll
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/quazip-master)

# ВАЖНО: Возвращаем ON для своих модулей, чтобы они собирались в .so (dll)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

# 3. РАЗДЕЛЕНИЕ ПО ОС
set(PLATFORM_SOURCES "") # Инициализируем пустую переменную

if(WIN32)
    # Эта часть сработает ТОЛЬКО на Windows
    add_compile_definitions(IS_WINDOWS)
    message(STATUS "Настройка специфичных штук для Windows...")
    # Добавляем иконку приложения .exe
    set(PLATFORM_SOURCES src/gui/res/app_icon.rc)
elseif(UNIX AND NOT APPLE)
    # Эта часть сработает ТОЛЬКО на Linux
    add_compile_definitions(IS_LINUX)
    message(STATUS "Настройка специфичных штук для Linux...")
endif()

# 4. Подключаем наши модули
add_subdirectory(src/core)
add_subdirectory(src/gui)

# 5. Глобальные пути для include
include_directories(src/core/include)

message(STATUS "Build system: ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMake конфигурация завершена")

if(APPLE)
    # Копируем портативный Mac-питон (из data/python/bin/...) внутрь бандла
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/python/"
            DESTINATION "FinWizard.app/Contents/Resources/python"
            COMPONENT Runtime)
endif()
