cmake_minimum_required(VERSION 3.24)
project(pixel_roguelike LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_property(PR_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT PR_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PR_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(PR_TSAN "Build with ThreadSanitizer (GCC/Clang; not together with PR_SANITIZE)" OFF)
option(PR_NATIVE "Optimise for the build machine's CPU (-march=native)" OFF)
option(PR_USE_SYSTEM_SDL "Use an installed SDL3 instead of fetching it" OFF)
option(PR_BUILD_TESTS "Build the test suite" ON)
option(PR_BUILD_BENCH "Build the benchmark" ON)
option(PR_WERROR "Treat warnings as errors (CI)" OFF)

# ---------------------------------------------------------------------------
# SDL3
# ---------------------------------------------------------------------------
if(PR_USE_SYSTEM_SDL)
  find_package(SDL3 REQUIRED CONFIG)
else()
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  # Subsystems we do not use (yet). Keeps the build small and fast.
  set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
  set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
  set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
  set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
  set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
  set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
  set(SDL_POWER OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(SDL3)
endif()

# ---------------------------------------------------------------------------
# Common compile settings for our own targets
# ---------------------------------------------------------------------------
function(pr_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(PR_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
      -Wmissing-prototypes -Wdouble-promotion)
    if(PR_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(PR_NATIVE)
      target_compile_options(${target} PRIVATE -march=native)
    endif()
    if(PR_SANITIZE)
      # -O1 in Debug, as the ASan docs advise: several times faster than -O0, same checks.
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
        $<$<CONFIG:Debug>:-O1>)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    if(PR_TSAN)
      target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# pr_sim: headless simulation library (src/core + src/sim)
# ---------------------------------------------------------------------------
add_library(pr_sim STATIC
  src/core/core.c
  src/core/jobs.c
  src/sim/elements.c
  src/sim/world.c
  src/sim/sim.c
  src/sim/pressure.c
  src/sim/heat.c
  src/sim/particle.c
  src/sim/creature.c
  src/sim/weapon.c
  src/sim/enemy.c
  src/sim/path.c
  src/sim/pickup.c
  src/sim/edit.c
  src/sim/scene.c
)
target_include_directories(pr_sim PUBLIC src)
target_link_libraries(pr_sim PUBLIC SDL3::SDL3)
if(NOT MSVC)
  target_link_libraries(pr_sim PUBLIC m)
endif()
pr_configure_target(pr_sim)
if(PR_SANITIZE AND NOT MSVC)
  target_link_options(pr_sim INTERFACE -fsanitize=address,undefined)
endif()
if(PR_TSAN AND NOT MSVC)
  target_link_options(pr_sim INTERFACE -fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# The game
# ---------------------------------------------------------------------------
add_executable(pixel-roguelike
  src/main.c
  src/game/app.c
  src/render/render.c
  src/ui/ui.c
)
target_link_libraries(pixel-roguelike PRIVATE pr_sim)
pr_configure_target(pixel-roguelike)

# ---------------------------------------------------------------------------
# Tests and benchmark
# ---------------------------------------------------------------------------
if(PR_BUILD_TESTS)
  enable_testing()
  add_executable(pr-tests
    tests/test_main.c
    tests/test_util.c
    tests/test_sim.c
    tests/test_edit.c
    tests/test_heat.c
    tests/test_gas.c
    tests/test_particle.c
    tests/test_creature.c
    tests/test_weapon.c
    tests/test_combat.c
    tests/test_powder.c
    tests/test_reaction.c
  )
  target_link_libraries(pr-tests PRIVATE pr_sim)
  pr_configure_target(pr-tests)
  # One CTest test per RUN(...) line in test_main.c, so `ctest -j` runs them in parallel.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS tests/test_main.c)
  file(STRINGS tests/test_main.c pr_test_lines REGEX "^ *RUN\\(test_[a-z0-9_]+\\);")
  foreach(line IN LISTS pr_test_lines)
    string(REGEX REPLACE "^ *RUN\\((test_[a-z0-9_]+)\\);.*$" "\\1" name "${line}")
    add_test(NAME ${name} COMMAND pr-tests --exact ${name})
  endforeach()
endif()

if(PR_BUILD_BENCH)
  add_executable(pr-bench bench/bench.c)
  target_link_libraries(pr-bench PRIVATE pr_sim)
  pr_configure_target(pr-bench)
endif()
