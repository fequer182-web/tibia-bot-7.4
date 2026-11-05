cmake_minimum_required(VERSION 3.20)
project(CromeUniversalV7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(CONFIG Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${CONFIG}" CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
endforeach()

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(crome_core STATIC
    core/profile/ProfileLoader.cpp
    core/profile/ProfileValidator.cpp
    core/profile/ProfileRegistry.cpp
    core/readers/ProfileBackedPlayerReader.cpp
    core/runtime/WindowsProcessMemory.cpp
    core/runtime/ClientSession.cpp
    app/ActionScheduler.cpp
    app/CooldownManager.cpp
    app/BotController.cpp
    modules/healing/HealingEngine.cpp
    modules/support/SupportEngine.cpp
    modules/runemaker/RuneMakerEngine.cpp
    modules/targeting/TargetingEngine.cpp
    modules/cavebot/CavebotEngine.cpp
    modules/looter/LooterEngine.cpp
)

target_include_directories(crome_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crome_core PUBLIC nlohmann_json::nlohmann_json)

if(NOT WIN32)
    message(FATAL_ERROR "Crome UI atualmente e destinada ao Windows.")
endif()

FetchContent_Declare(
    imgui_src
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui_src)

add_executable(Crome WIN32
    ui/main.cpp
    ${imgui_src_SOURCE_DIR}/imgui.cpp
    ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(Crome PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${imgui_src_SOURCE_DIR}
    ${imgui_src_SOURCE_DIR}/backends
)

target_compile_definitions(Crome PRIVATE UNICODE _UNICODE)
target_link_libraries(Crome PRIVATE crome_core opengl32 gdi32 imm32 dwmapi user32 psapi comdlg32 windowscodecs ole32 winhttp)
set_target_properties(Crome PROPERTIES OUTPUT_NAME "Crome")

# Ferramentas extras ficam desligadas por padrao para manter o build principal limpo.
option(CROME_BUILD_DEV_TOOLS "Build developer diagnostic tools" OFF)
if(CROME_BUILD_DEV_TOOLS)
    add_executable(CromeCoreCli app/main.cpp)
    target_link_libraries(CromeCoreCli PRIVATE crome_core)

    add_executable(profile_inspector tools/profile_inspector.cpp)
    target_link_libraries(profile_inspector PRIVATE crome_core)

    add_executable(cavebot_runtime_smoke tests/cavebot_runtime_smoke.cpp)
    target_include_directories(cavebot_runtime_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Native Olders runtime restored from the previously validated Crome SDK v21.6.
add_library(CromeRuntime SHARED
    native_runtime/internal/olders_walker_test.cpp
)
target_include_directories(CromeRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/native_runtime
)
target_link_libraries(CromeRuntime PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(CromeRuntime PROPERTIES OUTPUT_NAME "CromeRuntime" PREFIX "")
