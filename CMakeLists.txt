cmake_minimum_required(VERSION 3.20)
project(FinWizard VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WIN32_EXECUTABLE ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)

# ====================== QuaZip ======================
set(QUAZIP_QT_MAJOR_VERSION 6 CACHE STRING "" FORCE)
set(QUAZIP_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

if(WIN32)
    set(QUAZIP_ENABLE_BZIP2 OFF CACHE BOOL "" FORCE)
    set(QUAZIP_USE_QT_ZLIB OFF CACHE BOOL "" FORCE)

    set(ZLIB_FOUND TRUE)
    set(ZLIB_INCLUDE_DIR "C:/Qt/6.11.1/mingw_64/include/QtZlib" CACHE PATH "" FORCE)
    # Возвращаем железный путь к статике
    set(ZLIB_LIBRARY "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/lib/libz.a" CACHE FILEPATH "" FORCE)
else()
    set(QUAZIP_USE_QT_ZLIB OFF CACHE BOOL "" FORCE)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/quazip-master)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

# ====================== microtar ======================
add_library(microtar STATIC
    thirdparty/microtar/src/microtar.c
    thirdparty/microtar/src/microtar.h
)
# Это оставляем для порядка
target_include_directories(microtar PUBLIC thirdparty/microtar/src)

# ====================== Наши модули ======================
add_subdirectory(src/core)
add_subdirectory(src/gui)

include_directories(
    src/core/include
    thirdparty/microtar/src
)

if(WIN32)
    if(TARGET FinWizardGui)
        # 1. Скармливаем либу напрямую через таргет линковки
        target_link_libraries(FinWizardGui PRIVATE "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/lib/libz.a")

        # 2. ЯДЕРНЫЙ ХАК: Алиасим импорты на статические символы.
        # Линкер заменяет __imp_z_gzopen на обычный gzopen прямо в процессе линковки EXE!
        target_link_options(FinWizardGui PRIVATE
            "-Wl,--defsym,__imp_z_gzopen=gzopen"
            "-Wl,--defsym,__imp_z_gzdopen=gzdopen"
            "-Wl,--defsym,__imp_z_gzflush=gzflush"
            "-Wl,--defsym,__imp_z_gzclose=gzclose"
            "-Wl,--defsym,__imp_z_gzread=gzread"
            "-Wl,--defsym,__imp_z_gzwrite=gzwrite"
        )
    endif()
endif()

if(WINDOWS)
    set_target_properties(FinWizardGui PROPERTIES WIN32_EXECUTABLE TRUE)

    # Автоматический запуск windeployqt при CMake Install
    qt_generate_deploy_app_script(
        TARGET FinWizardGui
        OUTPUT_SCRIPT deploy_script
    )
    install(TARGETS FinWizardGui RUNTIME DESTINATION bin)
    install(SCRIPT ${deploy_script})
endif()

message(STATUS "Build system: ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMake configuration completed successfully")