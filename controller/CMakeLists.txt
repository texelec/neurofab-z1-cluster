# V2 Hardware (16 nodes) - Current/Default
if(BUILD_HW_V2)
    add_executable(controller_16
        controller_main.c
        w5500_eth.c
        z1_http_api.c
        ../common/core_queue/core_queue.c
    )

    # Set board to Pico 2 (RP2350)
    pico_set_binary_type(controller_16 copy_to_ram)
    set_target_properties(controller_16 PROPERTIES PICO_BOARD pico2)

    # Hardware version and stack size
    target_compile_definitions(controller_16 PRIVATE
        HW_V2
        PICO_STACK_SIZE=0x1000  # 4KB stack per core
    )

    target_link_libraries(controller_16
        pico_stdlib
        pico_multicore
        hardware_pio
        hardware_dma
        hardware_gpio
        hardware_spi
        hardware_vreg
        z1_onyx_bus
        z1_broker
        psram
        oled
        sd_card_stub
    )

    # Enable USB output
    pico_enable_stdio_usb(controller_16 1)
    pico_enable_stdio_uart(controller_16 0)

    # Create UF2/BIN/HEX outputs
    pico_add_extra_outputs(controller_16)
endif()

# V1 Hardware (12 nodes) - Legacy
if(BUILD_HW_V1)
    add_executable(controller_12
        controller_main.c
        w5500_eth.c
        z1_http_api.c
        ../common/core_queue/core_queue.c
    )

    # Set board to Pico 2 (RP2350)
    pico_set_binary_type(controller_12 copy_to_ram)
    set_target_properties(controller_12 PROPERTIES PICO_BOARD pico2)

    # Hardware version and stack size
    target_compile_definitions(controller_12 PRIVATE
        HW_V1
        PICO_STACK_SIZE=0x1000  # 4KB stack per core
    )

    target_link_libraries(controller_12
        pico_stdlib
        pico_multicore
        hardware_pio
        hardware_dma
        hardware_gpio
        hardware_spi
        hardware_vreg
        z1_onyx_bus
        z1_broker
        psram
        sd_card_stub  # No OLED on V1
    )

    # Enable USB output
    pico_enable_stdio_usb(controller_12 1)
    pico_enable_stdio_uart(controller_12 0)

    # Create UF2/BIN/HEX outputs
    pico_add_extra_outputs(controller_12)
endif()

