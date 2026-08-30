cmake_minimum_required(VERSION 3.21)
project(midi-bouncer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependency: CLAP headers only (header-only, MIT). Fetched at configure time.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.6
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(clap)

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
add_library(midi-bouncer MODULE src/midi_bouncer.cpp)
target_link_libraries(midi-bouncer PRIVATE clap)

option(MIDI_BOUNCER_DEBUG_LOG "Write note-event diagnostics to <temp>/MIDI_BOUNCER.log" OFF)
if(MIDI_BOUNCER_DEBUG_LOG)
    target_compile_definitions(midi-bouncer PRIVATE MIDI_BOUNCER_DEBUG_LOG)
endif()
set_target_properties(midi-bouncer PROPERTIES
    OUTPUT_NAME "MIDI Bouncer"
    PREFIX ""
    SUFFIX ".clap")

if(APPLE)
    # On macOS a .clap is a bundle directory, not a bare dylib.
    set_target_properties(midi-bouncer PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.loowps.midi-bouncer"
        MACOSX_BUNDLE_BUNDLE_NAME "MIDI Bouncer"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    # CLAP bundles need a PkgInfo containing "BNDL????".
    add_custom_command(TARGET midi-bouncer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:midi-bouncer>"
        COMMAND ${CMAKE_COMMAND} -E echo "BNDL????"
                > "$<TARGET_BUNDLE_CONTENT_DIR:midi-bouncer>/PkgInfo")
endif()

# ---------------------------------------------------------------------------
# Convenience: copy the built plugin into the per-user CLAP folder.
#   cmake --build build --target install-local
# ---------------------------------------------------------------------------
if(WIN32)
    set(_clap_user_dir "$ENV{LOCALAPPDATA}/Programs/Common/CLAP")
elseif(APPLE)
    set(_clap_user_dir "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")
else()
    set(_clap_user_dir "$ENV{HOME}/.clap")
endif()

add_custom_target(install-local
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_clap_user_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:midi-bouncer>" "${_clap_user_dir}/"
    DEPENDS midi-bouncer
    VERBATIM
    COMMENT "Installing plugin to ${_clap_user_dir}")
