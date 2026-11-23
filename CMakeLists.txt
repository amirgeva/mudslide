cmake_minimum_required(VERSION 3.16)

project(Mudslide VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MUD_BUILD_GAME "Build the SDL2 game" ON)
option(MUD_BUILD_TESTS "Build the simulation tests" ON)
option(MUD_FETCH_SDL "Download and build SDL2 when it is not installed" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------- core (no SDL)
# Terrain, ballistics, match rules and the computer players.
add_library(mud_core STATIC
  src/core/terrain.cpp
  src/core/match.cpp
  src/core/ai.cpp
)
target_include_directories(mud_core PUBLIC src)

# ---------------------------------------------------------------- art (no SDL)
add_library(mud_art STATIC
  src/gfx/canvas.cpp
  src/gfx/art.cpp
  src/gfx/font.cpp
)
target_include_directories(mud_art PUBLIC src)
target_link_libraries(mud_art PUBLIC mud_core)

# ---------------------------------------------------------------- tests
if(MUD_BUILD_TESTS)
  enable_testing()
  add_executable(mud_tests tests/test_core.cpp)
  target_link_libraries(mud_tests PRIVATE mud_core)
  add_test(NAME core COMMAND mud_tests)
endif()

# ---------------------------------------------------------------- game
if(MUD_BUILD_GAME)
  # SDL2 from the system / vcpkg / Homebrew if available, otherwise built from source.
  find_package(SDL2 CONFIG QUIET)
  if(NOT SDL2_FOUND)
    if(NOT MUD_FETCH_SDL)
      message(FATAL_ERROR "SDL2 not found. Install it (e.g. libsdl2-dev, brew install sdl2, vcpkg) "
                          "or configure with -DMUD_FETCH_SDL=ON.")
    endif()
    message(STATUS "SDL2 not found - downloading and building it")
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
      URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz)
    FetchContent_MakeAvailable(SDL2)
  endif()

  if(TARGET SDL2::SDL2)
    set(MUD_SDL_TARGET SDL2::SDL2)
  elseif(TARGET SDL2::SDL2-static)
    set(MUD_SDL_TARGET SDL2::SDL2-static)
  elseif(DEFINED SDL2_LIBRARIES)
    # Very old sdl2-config.cmake files only provide variables.
    add_library(mud_sdl2 INTERFACE)
    target_include_directories(mud_sdl2 INTERFACE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(mud_sdl2 INTERFACE ${SDL2_LIBRARIES})
    set(MUD_SDL_TARGET mud_sdl2)
  else()
    message(FATAL_ERROR "Found SDL2 but no usable CMake target")
  endif()

  set(GAME_SOURCES
    src/app/main.cpp
    src/app/game.cpp
    src/app/audio.cpp
    src/app/save.cpp
  )

  if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
    # Embed the procedurally drawn icon in the .exe.
    add_executable(mud_icongen tools/icongen.cpp)
    target_link_libraries(mud_icongen PRIVATE mud_art)
    set(ICON_FILE ${CMAKE_CURRENT_BINARY_DIR}/mudslide.ico)
    add_custom_command(
      OUTPUT ${ICON_FILE}
      COMMAND mud_icongen ${ICON_FILE}
      DEPENDS mud_icongen
      COMMENT "Drawing application icon")
    set(RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/mudslide.rc)
    file(WRITE ${RC_FILE} "IDI_ICON1 ICON \"${ICON_FILE}\"\n")
    set_source_files_properties(${RC_FILE} PROPERTIES OBJECT_DEPENDS ${ICON_FILE})
    list(APPEND GAME_SOURCES ${RC_FILE} ${ICON_FILE})
  endif()

  add_executable(mudslide WIN32 ${GAME_SOURCES})
  target_compile_definitions(mudslide PRIVATE MUD_VERSION="${PROJECT_VERSION}")
  target_link_libraries(mudslide PRIVATE mud_core mud_art)
  if(TARGET SDL2::SDL2main)
    target_link_libraries(mudslide PRIVATE SDL2::SDL2main)
  endif()
  target_link_libraries(mudslide PRIVATE ${MUD_SDL_TARGET})

  # Put SDL2.dll next to the game on Windows.
  if(WIN32 AND TARGET ${MUD_SDL_TARGET})
    get_target_property(_sdl_type ${MUD_SDL_TARGET} TYPE)
    if(_sdl_type STREQUAL "SHARED_LIBRARY")
      add_custom_command(TARGET mudslide POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${MUD_SDL_TARGET}> $<TARGET_FILE_DIR:mudslide>)
    endif()
  endif()

  install(TARGETS mudslide RUNTIME DESTINATION bin)
endif()
