# dshtok - CMake mirror of the Makefile.
#
#   cmake -S . -B build-cmake
#   cmake --build build-cmake
#   ctest --test-dir build-cmake --output-on-failure
#
# Same layout as the Makefile: one static library holds the core translation
# units, the dshtok executable adds src/main.c, and every tests/test_*.c becomes
# a CTest case linked against the library without main.c.

cmake_minimum_required(VERSION 3.20)

project(dshtok VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(DSHTOK_WARNINGS
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

set(DSHTOK_CORE_SOURCES
    src/util.c
    src/json.c
    src/ledger.c
    src/format.c
    src/path.c
    src/report.c)

add_library(dshtok_core STATIC ${DSHTOK_CORE_SOURCES})
target_include_directories(dshtok_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(dshtok_core PRIVATE ${DSHTOK_WARNINGS})

add_executable(dshtok src/main.c)
target_link_libraries(dshtok PRIVATE dshtok_core)
target_compile_options(dshtok PRIVATE ${DSHTOK_WARNINGS})

# libm is a separate library on glibc and folded into libc on macOS and MSVC.
find_library(DSHTOK_MATH_LIBRARY m)
if(DSHTOK_MATH_LIBRARY)
  target_link_libraries(dshtok PRIVATE ${DSHTOK_MATH_LIBRARY})
  target_link_libraries(dshtok_core INTERFACE ${DSHTOK_MATH_LIBRARY})
endif()

enable_testing()

file(GLOB DSHTOK_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c)
foreach(test_source IN LISTS DSHTOK_TEST_SOURCES)
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE dshtok_core)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_options(${test_name} PRIVATE ${DSHTOK_WARNINGS})
  add_test(NAME ${test_name}
           COMMAND ${test_name}
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# The shell smoke test is registered only when a bash is present AND actually
# runs. Finding the program is not enough on Windows: a stock system also has
# C:\Windows\System32\bash.exe, the WSL launcher, which fails outright when no
# distribution is installed. Ask it to do something trivial first, and skip the
# test rather than registering one that is known to be unrunnable.
find_program(DSHTOK_BASH_PROGRAM bash)
if(DSHTOK_BASH_PROGRAM)
  execute_process(COMMAND ${DSHTOK_BASH_PROGRAM} -c "exit 0"
                  RESULT_VARIABLE DSHTOK_BASH_RESULT
                  OUTPUT_QUIET ERROR_QUIET)
  if(DSHTOK_BASH_RESULT EQUAL 0)
    add_test(NAME integration
             COMMAND ${CMAKE_COMMAND} -E env DSHTOK_BIN=$<TARGET_FILE:dshtok>
                     ${DSHTOK_BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  else()
    message(STATUS "bash found at ${DSHTOK_BASH_PROGRAM} but cannot run; skipping the integration test")
  endif()
endif()
