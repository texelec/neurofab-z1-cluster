# V2 Hardware (16 nodes with auto-detection)
if(BUILD_HW_V2)
    add_executable(node_16
        node_main.c
        z1_snn_engine.c
    )

    # Set board to Pico 2 (RP2350)
    pico_set_binary_type(node_16 copy_to_ram)
    set_target_properties(node_16 PROPERTIES PICO_BOARD pico2)

    target_link_libraries(node_16
        pico_stdlib
        hardware_pio
        hardware_dma
        hardware_gpio
        hardware_vreg
        z1_onyx_bus
        z1_broker
        z1_commands
        psram
    )

    # Hardware version and stack size
    target_compile_definitions(node_16 PRIVATE
        HW_V2
        PICO_STACK_SIZE=0x1000  # 4KB stack
    )

    # Enable USB output
    pico_enable_stdio_usb(node_16 1)
    pico_enable_stdio_uart(node_16 0)

    # Create UF2/BIN/HEX outputs
    pico_add_extra_outputs(node_16)
endif()

# V1 Hardware (12 nodes with hardcoded IDs)
if(BUILD_HW_V1)
    # Generate 12 individual node binaries (node_12_0 through node_12_11)
    foreach(NODE_ID RANGE 0 11)
        set(TARGET_NAME node_12_${NODE_ID})
        
        add_executable(${TARGET_NAME}
            node_main.c
            z1_snn_engine.c
        )

        # Set board to Pico 2 (RP2350)
        pico_set_binary_type(${TARGET_NAME} copy_to_ram)
        set_target_properties(${TARGET_NAME} PROPERTIES PICO_BOARD pico2)

        target_link_libraries(${TARGET_NAME}
            pico_stdlib
            hardware_pio
            hardware_dma
            hardware_gpio
            hardware_vreg
            z1_onyx_bus
            z1_broker
            z1_commands
            psram
        )

        # Hardware version, hardcoded node ID, and stack size
        target_compile_definitions(${TARGET_NAME} PRIVATE
            HW_V1
            NODE_ID_HARDCODED=${NODE_ID}
            PICO_STACK_SIZE=0x1000  # 4KB stack
        )

        # Enable USB output
        pico_enable_stdio_usb(${TARGET_NAME} 1)
        pico_enable_stdio_uart(${TARGET_NAME} 0)

        # Create UF2/BIN/HEX outputs
        pico_add_extra_outputs(${TARGET_NAME})
    endforeach()
endif()






