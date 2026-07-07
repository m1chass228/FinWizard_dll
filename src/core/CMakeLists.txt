# src/core/CMakeLists.txt

add_library(core STATIC)   # или SHARED, если хочешь dll/so

# Все исходники и заголовки
target_sources(core
    PRIVATE
        src/pluginmanager.cpp
        src/pluginrepository.cpp
        src/pluginengine.cpp
    PUBLIC
        include/finwizard/pluginmanager.h
        include/finwizard/pluginrepository.h
        include/finwizard/pluginengine.h
        include/finwizard/iconfig.h
)

# Самое главное — добавляем путь к include/
# PUBLIC — чтобы он был виден и тем, кто линкует core
target_include_directories(core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Линкуем зависимости
target_link_libraries(core
    PUBLIC
        Qt6::Core
        QuaZip::QuaZip   # если у тебя QuaZip::QuaZip6 — замени
)

# Если соберёшь как SHARED — добавь экспорт символов
# target_compile_definitions(core PRIVATE FINWIZARD_CORE_EXPORT)
