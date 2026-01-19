# SD Card Library - wrapper around FatFS

add_library(sd_card STATIC
    sd_card.c
    z1_config.c
)

target_include_directories(sd_card PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

# Link to FatFS (the INTERFACE library compiles FatFS sources)
# This is the correct way per carlk3 examples
# NOTE: Since FatFs_SPI is INTERFACE, its sources compile here, which means
# we also need all its dependencies (hardware_rtc, hardware_spi, etc.)
target_link_libraries(sd_card PUBLIC
    FatFs_SPI
    pico_stdlib
    hardware_rtc
    hardware_spi
    hardware_dma
    hardware_clocks
)
