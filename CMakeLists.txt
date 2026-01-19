cmake_minimum_required(VERSION 3.13)

# Hardware version selection
option(BUILD_HW_V1 "Build for V1 hardware (12 nodes)" OFF)
option(BUILD_HW_V2 "Build for V2 hardware (16 nodes)" ON)

# Set platform to RP2350 (Pico 2) - must be before SDK import
# NOTE: Modern Pico SDK (2.0.0+) defaults to rp2350-arm-s (family ID 0xE48BFF59)
# RP2350B bootloader requires ABSOLUTE mode (family ID 0xE48BFF57)
# Workaround: Use pre-built firmware from backup or downgrade Pico SDK
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform")
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Pico SDK (must be before project)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# Use our pre-built picotool.exe that doesn't crash
set(picotool_FOUND TRUE)
set(PICOTOOL_EXECUTABLE "${CMAKE_SOURCE_DIR}/build_tools/picotool.exe" CACHE FILEPATH "Path to picotool")
set(PICOTOOL_TEST OFF CACHE BOOL "Disable picotool version test" FORCE)

project(z1_onyx_cluster C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# Disable picotool coprodis (crashes with underscores in path)
# This is done AFTER pico_sdk_init so we can override the function
macro(pico_add_dis_output TARGET)
    # Override to skip the coprodis step - just do objdump
    set(output_path "${CMAKE_CURRENT_BINARY_DIR}/")
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_OBJDUMP} -h $<TARGET_FILE:${TARGET}> > ${output_path}$<IF:$<BOOL:$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>>,$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>,$<TARGET_PROPERTY:${TARGET},NAME>>.dis
        COMMAND ${CMAKE_OBJDUMP} -d $<TARGET_FILE:${TARGET}> >> ${output_path}$<IF:$<BOOL:$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>>,$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>,$<TARGET_PROPERTY:${TARGET},NAME>>.dis
        VERBATIM
    )
endmacro()

# Common libraries
add_subdirectory(common/z1_onyx_bus)
add_subdirectory(common/z1_broker)
add_subdirectory(common/z1_commands)
add_subdirectory(common/psram)
add_subdirectory(common/oled)
add_subdirectory(common/FatFs_SPI)
add_subdirectory(common/sd_card)

# Bootloader (Phase 1: OTA dual-partition firmware)
add_subdirectory(bootloader)

# Controller and Node projects
add_subdirectory(controller)
add_subdirectory(node)

