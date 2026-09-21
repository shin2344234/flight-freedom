cmake_minimum_required(VERSION 3.20)
project(FlightProbe LANGUAGES CXX)

# Flight Freedom: an ASI plugin for Crimson Desert 2.02.00 that lifts the
# flying ceiling and the region block list behind no-fly zones and the Abyss
# summon refusal. The target is still called FlightProbe inside CMake; the
# file it produces is FlightFreedom.asi. Probe=1 in the ini turns the research
# hooks back on.

if(NOT WIN32)
    message(FATAL_ERROR "Flight Probe targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT, no redist needed

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# MinHook is here only for its HDE64 length decoder, which farhook uses to
# measure the bytes it steals. No MinHook hook is ever created.
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# FlightFreedom.ini's own bytes, as a header, so a DMM install (which is the
# plugin alone) can have the documented ini written beside it on first run
# without a second copy of the text going out of step with the first.
set(FP_INI_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ini_default.h")
add_custom_command(
    OUTPUT "${FP_INI_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} "-DIN=${CMAKE_CURRENT_SOURCE_DIR}/FlightFreedom.ini"
                             "-DOUT=${FP_INI_HEADER}"
                             -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_ini.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/FlightFreedom.ini"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_ini.cmake"
    COMMENT "Embedding FlightFreedom.ini"
    VERBATIM
)

add_library(FlightProbe SHARED
    "${FP_INI_HEADER}"
    src/dllmain.cpp
    src/core/mod.cpp
    src/core/log.cpp
    src/core/paths.cpp
    src/game/mem.cpp
    src/game/farhook.cpp
    src/game/tables.cpp
    src/game/conditions.cpp
    src/game/sites.cpp
    src/game/takeoff.cpp
)

target_include_directories(FlightProbe PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated" ${minhook_SOURCE_DIR}/src/hde)
target_link_libraries(FlightProbe PRIVATE minhook)
target_compile_definitions(FlightProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(FlightProbe PRIVATE /utf-8 /W3)

set_target_properties(FlightProbe PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "FlightFreedom")

if(MSVC)
    target_compile_options(FlightProbe PRIVATE /guard:cf)
    target_link_options(FlightProbe PRIVATE /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf)
endif()

# Test harness for the site hooks: hooks functions inside itself.
add_executable(SitesTest
    tests/sites_test.cpp
    src/core/log.cpp
    src/core/paths.cpp
    src/game/mem.cpp
    src/game/farhook.cpp
    src/game/tables.cpp
    src/game/conditions.cpp
    src/game/sites.cpp
    src/game/takeoff.cpp
)
target_include_directories(SitesTest PRIVATE src ${minhook_SOURCE_DIR}/src/hde)
target_link_libraries(SitesTest PRIVATE minhook)
target_compile_definitions(SitesTest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(SitesTest PRIVATE /utf-8 /W3 /EHsc)

add_custom_command(TARGET FlightProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:FlightProbe>" "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/FlightFreedom.ini" "${CMAKE_SOURCE_DIR}/dist/"
    COMMENT "Staging dist/"
    VERBATIM
)
