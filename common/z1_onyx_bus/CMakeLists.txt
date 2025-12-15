# Z1 Onyx Bus - Source-Synchronous Multi-Master Protocol (Phase 1)
# Note: PIO headers pre-generated manually with pioasm to avoid build system issues

add_library(z1_onyx_bus
    z1_bus.c
)

target_include_directories(z1_onyx_bus PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(z1_onyx_bus
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_gpio
)
