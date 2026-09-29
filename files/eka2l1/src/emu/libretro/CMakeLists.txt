if (SF2000)
    add_library(eka2l1_libretro STATIC
        libretro.cpp
    )
else()
    add_library(eka2l1_libretro SHARED
        libretro.cpp
    )
endif()

target_include_directories(eka2l1_libretro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/emu/system/include
    ${CMAKE_SOURCE_DIR}/src/emu/kernel/include
    ${CMAKE_SOURCE_DIR}/src/emu/mem/include
    ${CMAKE_SOURCE_DIR}/src/emu/common/include
    ${CMAKE_SOURCE_DIR}/src/emu/services/include
    ${CMAKE_SOURCE_DIR}/src/emu/drivers/include
    ${CMAKE_SOURCE_DIR}/src/emu/config/include
    ${CMAKE_SOURCE_DIR}/src/emu/package/include
    ${CMAKE_SOURCE_DIR}/src/emu/vfs/include
    ${CMAKE_SOURCE_DIR}/src/emu/cpu/include
)

target_link_libraries(eka2l1_libretro PRIVATE
    common
    config
    cpu
    drivers
    epoc
    epockern
    epocloader
    epocpkg
    epocservs
    lunasvg
    sqlite3
    yaml-cpp
    stb
)

if (NOT SF2000)
    target_link_libraries(eka2l1_libretro PRIVATE SDL2)
endif()

set_target_properties(eka2l1_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME eka2l1_libretro
)
