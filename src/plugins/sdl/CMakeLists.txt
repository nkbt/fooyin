find_package(SDL2 QUIET)

if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found; skipping sdl plugin.")
    return()
endif()

create_fooyin_plugin_internal(
    sdl
    DEPENDS #
        Fooyin::Core
        ${SDL2_LIBRARIES}
    SOURCES #
        sdlaudiosubsystem.cpp
        sdlaudiosubsystem.h
        sdloutput.cpp
        sdloutput.h
        sdlplugin.cpp
        sdlplugin.h
)

if(APPLE)
    find_package(SDL3 QUIET CONFIG)
    if(TARGET SDL3::SDL3-shared)
        # SDL2-compat loads SDL3 dynamically, so macdeployqt cannot find this runtime dependency.
        install(CODE "
            file(REAL_PATH \"$<TARGET_FILE:SDL3::SDL3-shared>\" sdl3_library)
            file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${LIB_INSTALL_DIR}/libSDL3.dylib\")
            file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${LIB_INSTALL_DIR}\" TYPE FILE FILES \"\${sdl3_library}\" RENAME libSDL3.dylib)
        " COMPONENT fooyin)
    endif()
endif()
