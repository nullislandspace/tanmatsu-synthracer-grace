cmake_minimum_required(VERSION 3.16)

# Use the RISC-V cross-compiler toolchain
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/toolchain-app.cmake")
endif()

project(app C)

# App source files — add your .c files here
set(APP_SOURCES
    main/main.c
    main/game_ui.c
    main/keybind_ui.c
    main/backdrop.c
    main/hud.c
    main/screens.c
    main/play_states.c
    main/crt0.c
    main/synthwave.c
    main/icons.c
    main/input.c
    main/game.c
    main/world.c
    main/render.c
    main/obstacle.c
    main/objects/cube.c
    main/objects/wall.c
    main/objects/booster.c
    main/objects/bridge.c
    main/objects/checkpoint.c
    main/objects/flipping_cube.c
    main/objects/jump_booster.c
    main/objects/ramp.c
    main/objects/restarea_marker.c
    main/objects/shield.c
    main/objects/synthengine_sign.c
    main/objects/tri.c
    main/areas/pixel_field.c
    main/areas/big_blocks.c
    main/areas/gateways.c
    main/areas/bridges.c
    main/areas/dynamic_passage.c
    main/areas/dynamic_gateway.c
    main/areas/simple_platform.c
    main/areas/synthengine_ad.c
    main/areas/rest.c
    main/save.c
    main/save_nbt.c
    main/audio_settings.c
    main/controls_settings.c
    main/attachments.c
    main/sfx/sfx_engine_hum.c
    main/sfx/sfx_pickup_ding.c
    main/sfx/sfx_pickup_plink.c
    main/sfx/sfx_crash.c
    main/sfx/sfx_scrape.c
    main/sfx/sfx_cube_bump.c
    main/sfx/sfx_gong.c
)

# Include directories — add paths to your headers here
set(APP_INCLUDES
    main
    include
    include/esp_additions/include
)

# Compile app sources as a relocatable object library
add_library(app_obj OBJECT ${APP_SOURCES})
target_include_directories(app_obj PRIVATE ${APP_INCLUDES})
target_compile_options(app_obj PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -Os -fPIC
    -ffunction-sections -fdata-sections -fno-common
    -D__thread=
    -DESP_PLATFORM
)

# ESP-IDF normally force-includes FreeRTOS headers for all components
target_compile_options(app_obj PRIVATE
    "SHELL:-include freertos/FreeRTOS.h"
    "SHELL:-include freertos/semphr.h"
    "SHELL:-include freertos/task.h"
    "SHELL:-include freertos/queue.h"
    "SHELL:-include freertos/event_groups.h"
)

# SynthEngine3D engine component (see synthengine3D/ and
# devdocs/engine-extraction.md). In this plain-CMake build it provides a
# relocatable OBJECT library; add_subdirectory MUST precede the link-script
# file(GENERATE) below so $<TARGET_OBJECTS:synthengine3d> resolves. Linking it
# to app_obj propagates the engine's PUBLIC include dir (synthengine3D/include)
# so game sources can #include "synthengine3d.h"; the engine's object files are
# folded into app.so explicitly in the link script (no double-counting -- the
# custom command, not CMake, performs the link).
add_subdirectory(synthengine3D)
target_link_libraries(app_obj PRIVATE synthengine3d)

# Collect object files into a list file, then link via a response file
# This avoids issues with generator expressions in custom commands
set(LINK_SCRIPT ${CMAKE_BINARY_DIR}/link_app.sh)
file(GENERATE OUTPUT ${LINK_SCRIPT} CONTENT
"#!/bin/sh
${CMAKE_C_COMPILER} \
    -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1 -mabi=ilp32f \
    -shared -nostdlib \
    -Wl,--gc-sections \
    -Wl,-Bsymbolic \
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/app_version.script \
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/app.ld \
    -Wl,-Map=${CMAKE_BINARY_DIR}/app.map \
    -L${CMAKE_CURRENT_SOURCE_DIR}/fakelib \
    $<JOIN:$<TARGET_OBJECTS:app_obj>, > \
    $<JOIN:$<TARGET_OBJECTS:synthengine3d>, > \
    -lall \
    -o ${CMAKE_BINARY_DIR}/app.so
")

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/app.so
    COMMAND sh ${LINK_SCRIPT}
    DEPENDS app_obj
        $<TARGET_OBJECTS:app_obj>
        synthengine3d
        $<TARGET_OBJECTS:synthengine3d>
        ${CMAKE_CURRENT_SOURCE_DIR}/app.ld
        ${CMAKE_CURRENT_SOURCE_DIR}/app_version.script
        ${CMAKE_CURRENT_SOURCE_DIR}/fakelib/liball.so
    COMMENT "Linking app.so"
)

add_custom_target(app ALL DEPENDS ${CMAKE_BINARY_DIR}/app.so)

# Print size after build
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_SIZE} ${CMAKE_BINARY_DIR}/app.so
    COMMENT "app.so size:"
)
