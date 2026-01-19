# Z1 Onyx Bootloader - CMake Configuration
# 
# Builds the fixed bootloader partition (0x00000000 - 0x001FFFFF, 2MB)
# Contains: Matrix Bus, Broker, PSRAM, OTA engine, Flash programming
# Does NOT contain: SNN engine (lives in application partition)
#
# Target outputs:
# - bootloader_16.elf (for debugging)
# - bootloader_16.uf2 (for initial flash via USB bootloader)

cmake_minimum_required(VERSION 3.13)

# Include Pico SDK
set(PICO_BOARD pico2 CACHE STRING "Board type")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(z1_bootloader C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize Pico SDK
pico_sdk_init()

# =========================================================================
# V2 Hardware Bootloader (16-node current)
# =========================================================================
if(BUILD_HW_V2)
    # Bootloader executable
    add_executable(bootloader_16
        bootloader_main.c
        ota_engine.c
    )

# Link Pico SDK libraries
target_link_libraries(bootloader_16
    pico_stdlib
    pico_multicore
    pico_bootrom
    hardware_flash
    hardware_sync
    hardware_gpio
    hardware_pio
    hardware_dma
    hardware_watchdog
)

# Link Z1 common libraries
target_link_libraries(bootloader_16
    z1_onyx_bus
    z1_broker
    psram
)

# CRITICAL: Define BOOTLOADER_BUILD for minimal broker configuration
# This reduces broker queue size from 97KB to ~10KB (no spike queue)
target_compile_definitions(bootloader_16 PRIVATE
    BOOTLOADER_BUILD=1
    HW_V2=1
)

# Include directories
target_include_directories(bootloader_16 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../common
    ${CMAKE_CURRENT_LIST_DIR}/../common/z1_onyx_bus
    ${CMAKE_CURRENT_LIST_DIR}/../common/z1_broker
    ${CMAKE_CURRENT_LIST_DIR}/../common/z1_commands
    ${CMAKE_CURRENT_LIST_DIR}/../common/psram
)

# Bootloader runs from flash (not RAM) - simpler and actually works
# Partition table approach requires tiny SRAM bootloader which doesn't fit our needs
# pico_set_linker_script(bootloader_16 ${CMAKE_CURRENT_LIST_DIR}/bootloader_memmap.ld)

# CRITICAL: RP2350B requires copy_to_ram for correct boot!
pico_set_binary_type(bootloader_16 copy_to_ram)

# Embed partition table (512KB bootloader partition avoids E10 errata)
# E10 errata: address 0x10ffff00 cannot be in partition
# 512KB partition (0x10000000-0x1007FFFF) ends BEFORE 0x10ffff00!
pico_embed_pt_in_binary(bootloader_16 ${CMAKE_CURRENT_LIST_DIR}/z1-pt.json)

# Create absolute UF2 (bootloader uses absolute addressing)
pico_set_uf2_family(bootloader_16 "absolute")

# Use custom linker script for bootloader partition (commented out for now - use default)
# pico_set_linker_script(bootloader_16 ${CMAKE_CURRENT_LIST_DIR}/bootloader_memmap.ld)

# Enable USB output, disable UART
pico_enable_stdio_usb(bootloader_16 1)
pico_enable_stdio_uart(bootloader_16 0)

# Add version string
target_compile_definitions(bootloader_16 PRIVATE
    BOOTLOADER_VERSION_MAJOR=1
    BOOTLOADER_VERSION_MINOR=0
    BOOTLOADER_VERSION_PATCH=0
    PICO_FLASH_SIZE_BYTES=16777216  # 16MB W25Q128JV
)

# Generate BIN and UF2 using our Python script (doesn't need picotool)
add_custom_command(TARGET bootloader_16 POST_BUILD
    COMMAND arm-none-eabi-objcopy -Obinary
        $<TARGET_FILE:bootloader_16>
        ${CMAKE_CURRENT_BINARY_DIR}/bootloader_16.bin
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/build_tools/elf2uf2.py
        $<TARGET_FILE:bootloader_16>
        ${CMAKE_CURRENT_BINARY_DIR}/bootloader_16.uf2
    COMMENT "Creating BIN and UF2 for bootloader_16"
    VERBATIM
)

# Print memory usage
add_custom_command(TARGET bootloader_16 POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:bootloader_16>
)
endif()  # BUILD_HW_V2

# =========================================================================
# V1 Hardware Bootloader (12-node legacy) - Single bootloader, app has hardcoded IDs
# =========================================================================
if(BUILD_HW_V1)
    add_executable(bootloader_12
        bootloader_main.c
        ota_engine.c
    )

    pico_set_binary_type(bootloader_12 copy_to_ram)
    set_target_properties(bootloader_12 PROPERTIES PICO_BOARD pico2)

    target_link_libraries(bootloader_12
        pico_stdlib
        pico_multicore
        hardware_flash
        hardware_sync
        hardware_gpio
        hardware_pio
        hardware_dma
        hardware_watchdog
        z1_onyx_bus
        z1_broker
        z1_commands
        psram
    )

    pico_enable_stdio_usb(bootloader_12 1)
    pico_enable_stdio_uart(bootloader_12 0)

    target_compile_definitions(bootloader_12 PRIVATE
        HW_V1
        BOOTLOADER_SKIP_NODE_ID  # V1: Let app handle node ID
        BOOTLOADER_VERSION_MAJOR=1
        BOOTLOADER_VERSION_MINOR=0
        BOOTLOADER_VERSION_PATCH=0
        PICO_FLASH_SIZE_BYTES=16777216
    )

    pico_add_extra_outputs(bootloader_12)
    
    add_custom_command(TARGET bootloader_12 POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:bootloader_12>
    )
endif()
