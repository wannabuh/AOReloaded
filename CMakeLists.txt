cmake_minimum_required(VERSION 3.24)

project(AOReloaded
    VERSION 0.1.0
    DESCRIPTION "Client mod framework for Anarchy Online"
    LANGUAGES CXX
)

# ── Build settings ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The AO client is a 32-bit process. Abort early on misconfigured builds.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "AOReloaded must be built for x86 (32-bit). "
        "Current pointer size: ${CMAKE_SIZEOF_VOID_P} bytes. "
        "Use the x86 preset or pass -DCMAKE_C_COMPILER_TARGET=i686-pc-windows-msvc.")
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF)
endif()

# Static CRT — no redistributable dependency for end users.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ── The proxy DLL ───────────────────────────────────────────────────────────
add_library(AOReloaded SHARED
    src/dllmain.cpp
    src/proxy/version_proxy.cpp
    src/proxy/version_proxy.def
    src/core/logging.cpp
    src/core/laa_patch.cpp
    src/core/settings.cpp
    src/core/fps_cap.cpp
    src/ao/game_api.cpp
    src/ao/dvalue_dump.cpp
    src/hooks/hook_engine.cpp
    src/hooks/input_handler.cpp
    src/hooks/camera_hook.cpp
    src/hooks/numpad_fix.cpp
    src/hooks/timer_bar_drag.cpp
    src/hooks/bag_move.cpp
    src/hooks/alt_select.cpp
)

set_target_properties(AOReloaded PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    SUFFIX ".dll"
)

target_include_directories(AOReloaded PRIVATE src)
target_compile_definitions(AOReloaded PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

# ── Post-build: copy the DLL into the AO client directory ──────────────────
set(AO_CLIENT_DIR "${CMAKE_SOURCE_DIR}/../client" CACHE PATH
    "Path to the AO client directory (where AnarchyOnline.exe lives)")

if(EXISTS "${AO_CLIENT_DIR}/AnarchyOnline.exe")
    add_custom_command(TARGET AOReloaded POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:AOReloaded>
            "${AO_CLIENT_DIR}/"
        COMMENT "Deploying version.dll to ${AO_CLIENT_DIR}"
    )
    # Also copy PDB for debugging
    add_custom_command(TARGET AOReloaded POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_PDB_FILE:AOReloaded>
            "${AO_CLIENT_DIR}/"
        COMMENT "Deploying PDB to ${AO_CLIENT_DIR}"
    )
endif()
