cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(otc-api LANGUAGES CXX)

# Keep the final, runnable artifacts together regardless of target architecture.
# The architecture suffix prevents the x86 and x64 binaries from overwriting one
# another; CMake's per-configuration build tree remains an internal workspace.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OTC_ARCH_SUFFIX x64)
else()
    set(OTC_ARCH_SUFFIX Win32)
endif()
set(OTC_ARTIFACT_DIR "${CMAKE_SOURCE_DIR}/dist")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "otc-api is Windows-only (DLL injection + named pipe)")
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
endif()

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

file(GLOB OTC_CALIBRATION_JSON CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/configs/calibrations/pe-*.json")
set(OTC_EMBEDDED_CALIBRATIONS_CPP
    "${CMAKE_BINARY_DIR}/generated/embedded_calibrations.cpp")
add_custom_command(
    OUTPUT "${OTC_EMBEDDED_CALIBRATIONS_CPP}"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/configs/calibrations"
        "-DOUTPUT_FILE=${OTC_EMBEDDED_CALIBRATIONS_CPP}"
        -P "${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedCalibrations.cmake"
    DEPENDS ${OTC_CALIBRATION_JSON}
        "${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedCalibrations.cmake"
    VERBATIM
)

set(OTC_API_SOURCES
    ${OTC_EMBEDDED_CALIBRATIONS_CPP}
    src/core/dllmain.cpp
    src/core/logger.cpp
    src/core/memory.cpp
    src/core/build_id.cpp
    src/core/profile.cpp
    src/core/profile_store.cpp
    src/core/player_reader.cpp
    src/core/container_reader.cpp
    src/core/creature_reader.cpp
    src/core/map_reader.cpp
    src/core/actions.cpp
    src/core/corpse.cpp

    src/ipc/json_lines.cpp
    src/ipc/pipe_server.cpp
    src/ipc/commands.cpp
)

add_library(OTCApi SHARED ${OTC_API_SOURCES})
target_include_directories(OTCApi PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(OTCApi PRIVATE nlohmann_json::nlohmann_json user32)
set_target_properties(OTCApi PROPERTIES
    OUTPUT_NAME "OTCApi-${OTC_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OTC_ARTIFACT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OTC_ARTIFACT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

add_custom_command(TARGET OTCApi POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/configs/calibrations"
        "-DDEST_DIR=${OTC_ARTIFACT_DIR}/calibrations"
        -P ${CMAKE_SOURCE_DIR}/cmake/SeedCalibrations.cmake
    VERBATIM
)

add_executable(OTCInjector injector/main.cpp)
target_include_directories(OTCInjector PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(OTCInjector PROPERTIES
    OUTPUT_NAME "OTCInjector-${OTC_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OTC_ARTIFACT_DIR}"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)
target_link_libraries(OTCInjector PRIVATE psapi)

include(CTest)
if(BUILD_TESTING)
    add_executable(corpse_tracker_test tests/corpse_tracker_test.cpp)
    target_include_directories(corpse_tracker_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME corpse_tracker COMMAND corpse_tracker_test)
    add_executable(profile_store_test tests/profile_store_test.cpp
        src/core/profile.cpp src/core/profile_store.cpp ${OTC_EMBEDDED_CALIBRATIONS_CPP})
    target_include_directories(profile_store_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(profile_store_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME profile_store COMMAND profile_store_test)
    add_executable(container_calibration_test tests/container_calibration_test.cpp
        src/core/memory.cpp src/core/logger.cpp src/ipc/pipe_server.cpp src/ipc/json_lines.cpp)
    target_include_directories(container_calibration_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(container_calibration_test PRIVATE _ITERATOR_DEBUG_LEVEL=0)
    target_link_libraries(container_calibration_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME container_calibration COMMAND container_calibration_test)
    add_test(NAME seed_calibrations COMMAND ${CMAKE_COMMAND}
        "-DSEED_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/SeedCalibrations.cmake"
        -P ${CMAKE_SOURCE_DIR}/tests/seed_calibrations_test.cmake)
endif()

