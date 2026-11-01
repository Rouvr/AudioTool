cmake_minimum_required(VERSION 3.20)
project(AudioTool VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Place all build artifacts in build/bin and build/lib inside the project directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
endif()

include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Resolve %LOCALAPPDATA%/Rouvr/AudioTool/plugins for automatic plugin deployment
if(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Rouvr/AudioTool/plugins" AT_USER_PLUGINS_DIR)
else()
    set(AT_USER_PLUGINS_DIR "${CMAKE_BINARY_DIR}/Rouvr_AudioTool_LocalData/plugins")
endif()

# ----------------------------------------------------------------------------
# 1. Core Audio & Control Engine Library (at_engine)
# ----------------------------------------------------------------------------
add_library(at_engine STATIC
    src/common/AppPathsAndRegistry.cpp
    src/engine/DeviceManager.cpp
    src/engine/PluginManager.cpp
    src/engine/AudioGraphEngine.cpp
)

target_link_libraries(at_engine PUBLIC
    ole32
    avrt
    advapi32
    shell32
    uuid
)

# ----------------------------------------------------------------------------
# 2. Dynamic Node Plugin DLLs (Deployed to %LOCALAPPDATA%/Rouvr/AudioTool/plugins)
# ----------------------------------------------------------------------------

# 2a. VirtualInputNode.dll -> %LOCALAPPDATA%/Rouvr/AudioTool/plugins/Audio/Sources/
add_library(VirtualInputNode SHARED
    src/nodes_builtin/Audio/VirtualInputNode/VirtualInputNode.cpp
)
set_target_properties(VirtualInputNode PROPERTIES PREFIX "")
add_custom_command(TARGET VirtualInputNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Sources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:VirtualInputNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Sources/VirtualInputNode.dll"
    COMMENT "Deploying VirtualInputNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Sources/"
)

# 2b. OutputNode.dll -> %LOCALAPPDATA%/Rouvr/AudioTool/plugins/Audio/Outputs/
add_library(OutputNode SHARED
    src/nodes_builtin/Audio/OutputNode/OutputNode.cpp
)
set_target_properties(OutputNode PROPERTIES PREFIX "")
add_custom_command(TARGET OutputNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Outputs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OutputNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Outputs/OutputNode.dll"
    COMMENT "Deploying OutputNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Outputs/"
)

# 2c. TimerNode.dll -> %LOCALAPPDATA%/Rouvr/AudioTool/plugins/Control/Triggers/
add_library(TimerNode SHARED
    src/nodes_builtin/Control/TimerNode/TimerNode.cpp
)
set_target_properties(TimerNode PROPERTIES PREFIX "")
add_custom_command(TARGET TimerNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Control/Triggers"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:TimerNode>"
        "${AT_USER_PLUGINS_DIR}/Control/Triggers/TimerNode.dll"
    COMMENT "Deploying TimerNode.dll to ${AT_USER_PLUGINS_DIR}/Control/Triggers/"
)

# ----------------------------------------------------------------------------
# 3. Proof-of-Concept Headless Audio Backend CLI (at_engine_poc.exe)
# ----------------------------------------------------------------------------
add_executable(at_engine_poc
    src/app/main_poc.cpp
)
target_link_libraries(at_engine_poc PRIVATE at_engine)
add_dependencies(at_engine_poc VirtualInputNode OutputNode TimerNode)

# ----------------------------------------------------------------------------
# 4. Automated Verification Test Suite (at_poc_tests.exe)
# ----------------------------------------------------------------------------
enable_testing()

add_executable(at_poc_tests
    tests/poc_passthrough_test.cpp
)
target_link_libraries(at_poc_tests PRIVATE at_engine)
add_dependencies(at_poc_tests VirtualInputNode OutputNode TimerNode)

add_test(NAME AudioTool_PoC_Tests COMMAND at_poc_tests)

