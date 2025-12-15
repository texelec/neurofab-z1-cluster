add_library(psram
    psram_rp2350.c
)

target_link_libraries(psram
    pico_stdlib
    hardware_spi
    hardware_dma
)

target_include_directories(psram PUBLIC ${CMAKE_CURRENT_LIST_DIR})
