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
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends"
)

# Resolve %LOCALAPPDATA%/Rouvr/AudioTool/plugins for automatic plugin deployment
if(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Rouvr/AudioTool/plugins" AT_USER_PLUGINS_DIR)
else()
    set(AT_USER_PLUGINS_DIR "${CMAKE_BINARY_DIR}/Rouvr_AudioTool_LocalData/plugins")
endif()

# ----------------------------------------------------------------------------
# 1. Dear ImGui Library
# ----------------------------------------------------------------------------
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# ----------------------------------------------------------------------------
# 2. Core Audio & Control Engine Library (at_engine)
# ----------------------------------------------------------------------------
add_library(at_engine STATIC
    src/common/AppPathsAndRegistry.cpp
    src/common/Logger.cpp
    src/engine/DeviceManager.cpp
    src/engine/PluginManager.cpp
    src/engine/ProfileGraph.cpp
    src/engine/AudioEngine.cpp
    src/engine/AudioGraphEngine.cpp
    src/engine/AppAudioRouter.cpp
)

target_link_libraries(at_engine PUBLIC
    ole32
    avrt
    advapi32
    shell32
    uuid
    dbghelp
)

# ----------------------------------------------------------------------------
# 3. Dynamic Node Plugin DLLs (Deployed to %LOCALAPPDATA%/Rouvr/AudioTool/plugins)
# ----------------------------------------------------------------------------

# 3a. VirtualInputNode.dll -> Audio/Sources/
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

# 3b. VolumeNode.dll -> Audio/Processing/
add_library(VolumeNode SHARED
    src/nodes_builtin/Audio/VolumeNode/VolumeNode.cpp
)
set_target_properties(VolumeNode PROPERTIES PREFIX "")
add_custom_command(TARGET VolumeNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Processing"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:VolumeNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Processing/VolumeNode.dll"
    COMMENT "Deploying VolumeNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Processing/"
)

# 3c. OutputNode.dll -> Audio/Outputs/
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

# 3d. TimerNode.dll -> Control/Triggers/
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

# 3e. EqualizerNode.dll -> Audio/EQ/
add_library(EqualizerNode SHARED
    src/nodes_builtin/Audio/EqualizerNode/EqualizerNode.cpp
)
set_target_properties(EqualizerNode PROPERTIES PREFIX "")
add_custom_command(TARGET EqualizerNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/EQ"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:EqualizerNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/EQ/EqualizerNode.dll"
    COMMENT "Deploying EqualizerNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/EQ/"
)

# 3f. FilterNode.dll -> Audio/Filters/
add_library(FilterNode SHARED
    src/nodes_builtin/Audio/FilterNode/FilterNode.cpp
)
set_target_properties(FilterNode PROPERTIES PREFIX "")
add_custom_command(TARGET FilterNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Filters"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:FilterNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Filters/FilterNode.dll"
    COMMENT "Deploying FilterNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Filters/"
)

# 3g. CompressorNode.dll -> Audio/Dynamics/
add_library(CompressorNode SHARED
    src/nodes_builtin/Audio/CompressorNode/CompressorNode.cpp
)
set_target_properties(CompressorNode PROPERTIES PREFIX "")
add_custom_command(TARGET CompressorNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Dynamics"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:CompressorNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Dynamics/CompressorNode.dll"
    COMMENT "Deploying CompressorNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Dynamics/"
)

# 3h. LimiterNode.dll -> Audio/Dynamics/
add_library(LimiterNode SHARED
    src/nodes_builtin/Audio/LimiterNode/LimiterNode.cpp
)
set_target_properties(LimiterNode PROPERTIES PREFIX "")
add_custom_command(TARGET LimiterNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Dynamics"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:LimiterNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Dynamics/LimiterNode.dll"
    COMMENT "Deploying LimiterNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Dynamics/"
)

# 3i. NoiseGateNode.dll -> Audio/Dynamics/
add_library(NoiseGateNode SHARED
    src/nodes_builtin/Audio/NoiseGateNode/NoiseGateNode.cpp
)
set_target_properties(NoiseGateNode PROPERTIES PREFIX "")
add_custom_command(TARGET NoiseGateNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Dynamics"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:NoiseGateNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Dynamics/NoiseGateNode.dll"
    COMMENT "Deploying NoiseGateNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Dynamics/"
)

# 3j. ReverbNode.dll -> Audio/Effects/
add_library(ReverbNode SHARED
    src/nodes_builtin/Audio/ReverbNode/ReverbNode.cpp
)
set_target_properties(ReverbNode PROPERTIES PREFIX "")
add_custom_command(TARGET ReverbNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Effects"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:ReverbNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Effects/ReverbNode.dll"
    COMMENT "Deploying ReverbNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Effects/"
)

# 3k. DelayNode.dll -> Audio/Effects/
add_library(DelayNode SHARED
    src/nodes_builtin/Audio/DelayNode/DelayNode.cpp
)
set_target_properties(DelayNode PROPERTIES PREFIX "")
add_custom_command(TARGET DelayNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Effects"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:DelayNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Effects/DelayNode.dll"
    COMMENT "Deploying DelayNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Effects/"
)

# 3l. DistortionNode.dll -> Audio/Distortion/
add_library(DistortionNode SHARED
    src/nodes_builtin/Audio/DistortionNode/DistortionNode.cpp
)
set_target_properties(DistortionNode PROPERTIES PREFIX "")
add_custom_command(TARGET DistortionNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Distortion"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:DistortionNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Distortion/DistortionNode.dll"
    COMMENT "Deploying DistortionNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Distortion/"
)

# 3m. OverdriveNode.dll -> Audio/Distortion/
add_library(OverdriveNode SHARED
    src/nodes_builtin/Audio/OverdriveNode/OverdriveNode.cpp
)
set_target_properties(OverdriveNode PROPERTIES PREFIX "")
add_custom_command(TARGET OverdriveNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Distortion"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OverdriveNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Distortion/OverdriveNode.dll"
    COMMENT "Deploying OverdriveNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Distortion/"
)

# 3n. SaturationNode.dll -> Audio/Distortion/
add_library(SaturationNode SHARED
    src/nodes_builtin/Audio/SaturationNode/SaturationNode.cpp
)
set_target_properties(SaturationNode PROPERTIES PREFIX "")
add_custom_command(TARGET SaturationNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Distortion"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SaturationNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Distortion/SaturationNode.dll"
    COMMENT "Deploying SaturationNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Distortion/"
)

# 3o. BitcrusherNode.dll -> Audio/Distortion/
add_library(BitcrusherNode SHARED
    src/nodes_builtin/Audio/BitcrusherNode/BitcrusherNode.cpp
)
set_target_properties(BitcrusherNode PROPERTIES PREFIX "")
add_custom_command(TARGET BitcrusherNode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AT_USER_PLUGINS_DIR}/Audio/Distortion"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:BitcrusherNode>"
        "${AT_USER_PLUGINS_DIR}/Audio/Distortion/BitcrusherNode.dll"
    COMMENT "Deploying BitcrusherNode.dll to ${AT_USER_PLUGINS_DIR}/Audio/Distortion/"
)

# ----------------------------------------------------------------------------
# 4. Applications
# ----------------------------------------------------------------------------

# 4a. Standalone Native GUI Application (at_gui.exe)
add_executable(at_gui WIN32
    src/gui/CanvasRenderer.cpp
    src/gui/main_gui.cpp
)
target_link_libraries(at_gui PRIVATE at_engine imgui comdlg32)
add_dependencies(at_gui
    VirtualInputNode VolumeNode OutputNode TimerNode
    EqualizerNode FilterNode CompressorNode LimiterNode NoiseGateNode
    ReverbNode DelayNode DistortionNode OverdriveNode SaturationNode BitcrusherNode
)

# 4b. Headless CLI Engine Runner (at_engine_poc.exe)
add_executable(at_engine_poc
    src/app/main_poc.cpp
)
target_link_libraries(at_engine_poc PRIVATE at_engine)
add_dependencies(at_engine_poc
    VirtualInputNode VolumeNode OutputNode TimerNode
    EqualizerNode FilterNode CompressorNode LimiterNode NoiseGateNode
    ReverbNode DelayNode DistortionNode OverdriveNode SaturationNode BitcrusherNode
)

# ----------------------------------------------------------------------------
# 5. Automated Verification Test Suite (at_poc_tests.exe)
# ----------------------------------------------------------------------------
enable_testing()

add_executable(at_poc_tests
    tests/poc_passthrough_test.cpp
    src/gui/CanvasRenderer.cpp
)
target_link_libraries(at_poc_tests PRIVATE at_engine imgui)
add_dependencies(at_poc_tests
    VirtualInputNode VolumeNode OutputNode TimerNode
    EqualizerNode FilterNode CompressorNode LimiterNode NoiseGateNode
    ReverbNode DelayNode DistortionNode OverdriveNode SaturationNode BitcrusherNode
)

add_test(NAME AudioTool_PoC_Tests COMMAND at_poc_tests)
