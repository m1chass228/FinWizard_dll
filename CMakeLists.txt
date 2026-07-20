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
    # ВАЖНО: заголовки должны быть от ТОГО ЖЕ zlib, с которым линкуемся ниже
    # (libz.a из поставки MinGW-тулчейна), а не от Qt/include/QtZlib — тот
    # zlib приватный для Qt6Core.dll, его символы ждут DLL-импорт (__imp_*),
    # которого для обычной статической libz.a просто не существует. Раньше
    # это лечили через "-Wl,--defsym,__imp_z_gzopen=gzopen" ниже — но defsym
    # присваивает символу ЗНАЧЕНИЕ адреса функции, а не ячейку-указатель НА
    # него, так что "mov rax,[__imp_z_gzopen]; call rax" читал первые 8 байт
    # машинного кода gzopen как будто это адрес, и прыгал в мусор (см. отладку
    # 20.07 — SIGSEGV на 0xffffffffbad08949, ровно эти байты). При заголовках
    # от того же тулчейна gzopen линкуется как обычный статический символ,
    # никакого __imp_* вообще не генерируется — костыль ниже больше не нужен.
    set(ZLIB_INCLUDE_DIR "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/include" CACHE PATH "" FORCE)
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
        # Линкуем реальный статический zlib из тулчейна MinGW. Раньше здесь же
        # стоял блок "-Wl,--defsym,__imp_z_gzopen=gzopen" и еще 5 таких же —
        # он был нужен только из-за рассинхрона заголовков/библиотеки выше и
        # реально портил память в рантайме (см. комментарий у ZLIB_INCLUDE_DIR).
        # С правильными заголовками defsym не нужен вообще: gzopen и все
        # остальные линкуются как обычные статические символы.
        target_link_libraries(FinWizardGui PRIVATE "C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/lib/libz.a")
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