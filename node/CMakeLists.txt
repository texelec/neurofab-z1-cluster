# V2 Hardware (16 nodes with auto-detection)
if(BUILD_HW_V2)
    # =========================================================================
    # Application partition (for OTA dual-partition architecture)
    # Full node functionality - runs on top of bootloader
    # Built at standard flash address, bootloader uses rom_chain_image to boot
    # =========================================================================
    add_executable(node_app_16
        node_main.c
        z1_snn_engine.c
    )
    
    # Set board to Pico 2 (RP2350)
    set_target_properties(node_app_16 PROPERTIES PICO_BOARD pico2)
    
    # Override flash start address for app partition using linker --defsym
    # App is at 0x10080000 + 192-byte header = 0x100800C0
    # Vector table MUST be at 0x100800C0 for bootloader to find it
    target_link_options(node_app_16 PRIVATE
        "LINKER:--defsym=__flash_binary_start=0x100800C0"
        "LINKER:--section-start=.text=0x100800C0"
    )
    
    # Use SDK's standard IMAGE_DEF generation (HASH ONLY, NO SIGNATURE)
    pico_hash_binary(node_app_16)
    
    # Generate UF2, hex, and bin outputs
    pico_add_extra_outputs(node_app_16)
    
    target_link_libraries(node_app_16
        pico_stdlib
        hardware_pio
        hardware_dma
        hardware_gpio
        hardware_pwm
        hardware_vreg
        z1_onyx_bus
        z1_broker
        z1_commands
        psram
    )
    
    # Hardware version and app mode
    target_compile_definitions(node_app_16 PRIVATE
        HW_V2
        APP_PARTITION_MODE  # Flag to indicate we're running as app
        PICO_STACK_SIZE=0x800  # 2KB stack for app
    )
    
    # Custom linker script for app partition (runs at 0x102000C0, not 0x10000000)
    # TODO: Linker script needs mutex sections - for now rely on bootloader setup
    # pico_set_linker_script(node_app_16 ${CMAKE_CURRENT_LIST_DIR}/app_memmap.ld)
    
    # Enable USB output
    pico_enable_stdio_usb(node_app_16 1)
    pico_enable_stdio_uart(node_app_16 0)
    
    # NOTE: Don't use pico_add_extra_outputs - it calls picotool which crashes
    # We create .bin manually above, and build_dual.py handles UF2 conversion
endif()

# V1 Hardware (12 nodes with hardcoded IDs)
if(BUILD_HW_V1)
    # =========================================================================
    # Application partitions for V1 (for OTA dual-partition architecture)
    # Generate 12 individual app binaries (node_app_12_0 through node_app_12_11)
    # Same functionality as V2 but with hardcoded node IDs
    # =========================================================================
    foreach(NODE_ID RANGE 0 11)
        set(APP_TARGET_NAME node_app_12_${NODE_ID})
        
        add_executable(${APP_TARGET_NAME}
            node_main.c
            z1_snn_engine.c
        )
        
        # Set board to Pico 2 (RP2350)
        set_target_properties(${APP_TARGET_NAME} PROPERTIES PICO_BOARD pico2)
        
        # Override flash start address for app partition using linker --defsym
        # App is at 0x10080000 + 192-byte header = 0x100800C0
        target_link_options(${APP_TARGET_NAME} PRIVATE
            "LINKER:--defsym=__flash_binary_start=0x100800C0"
            "LINKER:--section-start=.text=0x100800C0"
        )
        
        # Use SDK's standard IMAGE_DEF generation (HASH ONLY, NO SIGNATURE)
        pico_hash_binary(${APP_TARGET_NAME})
        
        # Generate UF2, hex, and bin outputs
        pico_add_extra_outputs(${APP_TARGET_NAME})
        
        target_link_libraries(${APP_TARGET_NAME}
            pico_stdlib
            hardware_pio
            hardware_dma
            hardware_gpio
            hardware_pwm
            hardware_vreg
            z1_onyx_bus
            z1_broker
            z1_commands
            psram
        )
        
        # Hardware version, app mode, hardcoded node ID
        target_compile_definitions(${APP_TARGET_NAME} PRIVATE
            HW_V1
            APP_PARTITION_MODE
            NODE_ID_HARDCODED=${NODE_ID}
            PICO_STACK_SIZE=0x800  # 2KB stack for app
        )
        
        # Enable USB output
        pico_enable_stdio_usb(${APP_TARGET_NAME} 1)
        pico_enable_stdio_uart(${APP_TARGET_NAME} 0)
    endforeach()
endif()






