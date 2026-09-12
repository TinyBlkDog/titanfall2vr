cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# THE VERSION, AND IT IS DECLARED IN EXACTLY ONE PLACE.
#
# The zip name, the DLL's own startup banner and the git tag must be incapable
# of disagreeing, so all three read this line: version.h is generated from it,
# tools/package.ps1 greps it out of this file, and RELEASE-CHECKLIST requires
# the tag to match. Bump it here and nowhere else.
set(TF2VR_VERSION_TAG "v0.1.0")

# WHAT IT IS CALLED AND WHO WROTE IT, declared here for the same reason and
# generated into the same header. Northstar is told this name, the VR panel
# paints this name, and neither of them owns a copy of it.
#
# TF2VR_AUTHOR must match the name on the copyright line in LICENSE. It is the
# only string here a person could reasonably disagree about, so it is declared
# once rather than typed wherever it happened to be needed.
set(TF2VR_PRODUCT_NAME "Titanfall 2 VR")
set(TF2VR_AUTHOR       "TinyBlackDog")

string(REGEX MATCH "^v([0-9]+\\.[0-9]+\\.[0-9]+)" _tf2vr_version_match "${TF2VR_VERSION_TAG}")
if(NOT _tf2vr_version_match)
    message(FATAL_ERROR "TF2VR_VERSION_TAG must look like v<major>.<minor>.<patch>[-suffix]; got '${TF2VR_VERSION_TAG}'")
endif()
set(TF2VR_VERSION "${CMAKE_MATCH_1}")

project(Titanfall2VRPlugin VERSION ${TF2VR_VERSION} LANGUAGES CXX ASM_MASM)

# The commit this was configured at, for the banner. Best-effort: a source tree
# with no git, or a build that was not reconfigured after a commit, gets
# "unknown" or a stale sha respectively -- which is why the banner also prints
# the DLL's own write time, and that cannot go stale.
set(TF2VR_GIT_SHA "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _tf2vr_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_tf2vr_sha)
        set(TF2VR_GIT_SHA "${_tf2vr_sha}")
    endif()
endif()

# Once at configure time, so the header exists before anything compiles...
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

# ...and again on EVERY build, because a configure-time sha goes stale the
# moment you commit without touching this file: a DLL built on 2026-09-11 at
# 21:36 announced itself as a commit from hours earlier. See
# cmake/version_stamp.cmake. copy_if_different in that script means an
# unchanged commit does not force a rebuild of everything including version.h.
add_custom_target(tf2vr_version ALL
    COMMAND "${CMAKE_COMMAND}"
            -DTF2VR_VERSION_TAG=${TF2VR_VERSION_TAG}
            -DTF2VR_VERSION=${TF2VR_VERSION}
            -DTF2VR_PRODUCT_NAME=${TF2VR_PRODUCT_NAME}
            -DTF2VR_AUTHOR=${TF2VR_AUTHOR}
            -DTF2VR_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTF2VR_IN=${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h.in
            -DTF2VR_OUT=${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_stamp.cmake"
    COMMENT "Stamping version.h with the current commit"
    VERBATIM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetches and builds the OpenXR SDK's loader automatically at configure
# time - same proven pattern as C:\dev\doomvr-m1\CMakeLists.txt. No manual
# SDK install/OPENXR_SDK env var required. This project intentionally has
# no dependency on Northstar's private build targets.
include(FetchContent)
FetchContent_Declare(
    openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    # PINNED. `main` meant a stranger cloning this repo could build against a
    # different loader than the one every measurement here was taken with, and
    # the DLL hash would not be reproducible. This is the tag that was actually
    # in build/_deps when the alpha was cut.
    GIT_TAG release-1.1.62
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(openxr_sdk)
if(NOT TARGET openxr_loader)
    message(FATAL_ERROR "OpenXR-SDK was fetched, but target 'openxr_loader' was not created")
endif()

# ---------------------------------------------------------------------------
# Dear ImGui, vendored in-tree and pinned. See third_party/imgui/VERSION for the
# tag, the commit and why this is not FetchContent.
#
# A SEPARATE STATIC LIBRARY rather than sources bolted onto the plugin target,
# for two reasons. It keeps ImGui compiling under its own warning settings
# instead of ours, and -- the reason that matters for M0 -- with no call sites
# yet, /OPT:REF drops every unreferenced COMDAT at link, so M0 can claim to be
# inert and then PROVE it with the DLL size rather than asserting it.
#
# imgui_demo.cpp is deliberately absent (PLAN-VRMENU 1.5).
add_library(tf2vr_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(tf2vr_imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(tf2vr_imgui PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(tf2vr_imgui PUBLIC d3d11 dxgi user32 dwmapi)

add_library(titanfall2vr_plugin SHARED
    camera_hook.asm
    view_build_hook.asm
    src/camera/camera_hook.cpp
    src/camera/player_eye.cpp
    src/camera/player_eye_hook.cpp
    src/camera/placement_watchpoint.cpp
    src/hands/placement_pin.cpp
    src/hands/arms_collapse.cpp
    src/diag/mesh_census.cpp
    src/diag/bone_reader_watch.cpp
    src/diag/draw_item_census.cpp
    src/diag/vb_bind_census.cpp
    src/input/vr_input.cpp
    src/input/xinput_pad.cpp
    src/input/aim_cmd.cpp
    src/hands/use_target.cpp
    src/hud/world_marker.cpp
    src/diag/anchor_probe.cpp
    src/diag/frustum_census.cpp
    src/ads/ads_probe.cpp
    src/ads/ads_zoom.cpp
    src/ads/ads_lock.cpp
    src/hud/crosshair_scale.cpp
    src/ui/menu_overlay.cpp
    src/hud/hudwarp.cpp
    src/hud/rui_probe.cpp
    src/hud/rui_layer_probe.cpp
    src/hud/rui_hunt.cpp
    src/hud/rui_asset_dump.cpp
    src/hud/rui_immediate.cpp
    src/hud/lock_hud.cpp
    src/hands/viewmodel_visibility.cpp
    src/hands/viewmodel_instance.cpp
    src/camera/view_build_hook.cpp
    render_hook.asm
    src/render/stereo_experiment.cpp
    src/render/render_hook.cpp
    src/diag/jobpool_watch.cpp
    src/diag/stall_watchdog.cpp
    src/diag/jt_probe.cpp
    src/render/scene_reentry.cpp
    src/render/stereo_targets.cpp
    src/render/view_block_camera.cpp
    src/render/temporal_lever.cpp
    src/xr/render_resolution.cpp
    src/core/crash_recorder.cpp
    src/input/dpad_gesture.cpp
    src/engine/hook_registry.cpp
    src/hands/titan_state.cpp
    src/core/diagnostics.cpp
    src/render/d3d11_trace.cpp
    src/render/d3d11_entry_trace.cpp
    src/diag/aim_probe.cpp
    src/diag/aim_census.cpp
    src/hands/viewmodel_bones.cpp
    src/diag/bone_staging_probe.cpp
    squirrel_natives.asm
    src/hands/hand_pose.cpp
    src/hands/viewmodel_hide.cpp
    src/engine/squirrel_natives.cpp
    placement_hook.asm
    arms_bones.asm
    aim_cmd.asm
    crosshair_hook.asm
    rui_hook.asm
    studio_draw.asm
    src/render/camera_update_hook.cpp
    src/core/config.cpp
    src/engine/engine_cvars.cpp
    src/core/flat_harness.cpp
    src/hands/weapon_settings.cpp
    src/host/plugin.cpp
    src/render/present_hook.cpp
    src/diag/plugin_cost.cpp
    src/diag/xr_breadcrumb.cpp
    src/diag/resource_watch.cpp
    src/xr/xr_context.cpp
    src/xr/xr_input.cpp)

# The stamp must run BEFORE the plugin compiles, or the header it rewrites is
# read in the same build that rewrote it.
add_dependencies(titanfall2vr_plugin tf2vr_version)

# EVERY SUBDIRECTORY IS ON THE INCLUDE PATH, deliberately. The 2026-09-08 move
# from one flat src/ into subsystems is then a pure file move: not one #include
# line changes, so the diff is `git mv` plus this list and nothing else can hide
# in it. Qualifying the includes ("xr/xr_context.h") is a later, separate pass.
target_include_directories(titanfall2vr_plugin PRIVATE
    "${openxr_sdk_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    src src/host src/core src/engine src/xr src/render src/camera src/input
    src/hands src/hud src/ads src/ui src/diag)
# The renderdoc_app.h lookup that used to live here is gone with the explicit
# Start/EndFrameCapture path, which is closed. RenderDoc still works as an
# external capture tool, and the ID3DUserDefinedAnnotation markers that label
# our passes for it are plain D3D11 and need no RenderDoc headers.
target_compile_definitions(titanfall2vr_plugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11)
target_link_libraries(titanfall2vr_plugin PRIVATE d3d11 dxgi user32 tf2vr_imgui "$<LINK_ONLY:openxr_loader>")
set_target_properties(titanfall2vr_plugin PROPERTIES OUTPUT_NAME "titanfall2vr")

# A LINKER MAP, so a crash record's titanfall2vr.dll+0xRVA can be turned into a
# function name offline. 2026-09-08: a crash landed four frames deep in this DLL
# and there was no way to say WHICH four without one. Costs nothing at runtime.
target_link_options(titanfall2vr_plugin PRIVATE "/MAP:$<TARGET_FILE_DIR:titanfall2vr_plugin>/titanfall2vr.map")

# Deliberately opt-in: copying a DLL into a live game installation is never a
# side effect of a normal build.
set(TF2VR_PLUGIN_OUTPUT_DIR "" CACHE PATH "Optional Northstar plugins directory to receive the DLL")
if(TF2VR_PLUGIN_OUTPUT_DIR)
    add_custom_command(TARGET titanfall2vr_plugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TF2VR_PLUGIN_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:titanfall2vr_plugin>" "${TF2VR_PLUGIN_OUTPUT_DIR}")
endif()
