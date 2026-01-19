# V2 Hardware (16 nodes) - Current/Default
if(BUILD_HW_V2)
    add_executable(controller_16
        controller_main.c
        w5500_eth.c
        z1_http_api.c
        hw_config.c
        ../common/core_queue/core_queue.c
        ../common/sd_card/sd_card.c
        ../common/sd_card/z1_config.c
    )

    # Set board to Pico 2 (RP2350)
    pico_set_binary_type(controller_16 copy_to_ram)
    set_target_properties(controller_16 PROPERTIES PICO_BOARD pico2)

    # Hardware version and stack size
    target_compile_definitions(controller_16 PRIVATE
        HW_V2
        PICO_STACK_SIZE=0x1000  # 4KB stack per core
    )

    # Include sd_card wrapper headers AND hardware_rtc manually (SDK bug workaround)
    target_include_directories(controller_16 PRIVATE
        ../common/sd_card
        ${PICO_SDK_PATH}/src/rp2_common/hardware_rtc/include
    )

    target_link_libraries(controller_16
        pico_stdlib
        pico_multicore
        hardware_pio
        hardware_dma
        hardware_gpio
        hardware_pwm
        hardware_spi
        hardware_clocks
        hardware_vreg
        z1_onyx_bus
        z1_broker
        psram
        oled
        FatFs_SPI  # INTERFACE library - compiles FatFS sources with proper dependencies
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
        hw_config.c
        ../common/core_queue/core_queue.c
        ../common/sd_card/sd_card.c
        ../common/sd_card/z1_config.c
    )

    # Set board to Pico 2 (RP2350)
    pico_set_binary_type(controller_12 copy_to_ram)
    set_target_properties(controller_12 PROPERTIES PICO_BOARD pico2)

    # Hardware version and stack size
    target_compile_definitions(controller_12 PRIVATE
        HW_V1
        PICO_STACK_SIZE=0x1000  # 4KB stack per core
    )

    # Include sd_card wrapper headers AND hardware_rtc manually (SDK bug workaround)
    target_include_directories(controller_12 PRIVATE
        ../common/sd_card
        ${PICO_SDK_PATH}/src/rp2_common/hardware_rtc/include
    )

    target_link_libraries(controller_12
        pico_stdlib
        pico_multicore
        hardware_pio
        hardware_dma
        hardware_gpio
        hardware_pwm
        hardware_spi
        hardware_clocks
        hardware_vreg
        z1_onyx_bus
        z1_broker
        psram
        FatFs_SPI  # INTERFACE library - compiles FatFS sources with proper dependencies (no OLED on V1)
    )

    # Enable USB output
    pico_enable_stdio_usb(controller_12 1)
    pico_enable_stdio_uart(controller_12 0)

    # Create UF2/BIN/HEX outputs
    pico_add_extra_outputs(controller_12)
endif()

