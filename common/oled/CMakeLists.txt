add_library(oled
    ssd1306.c
)

target_link_libraries(oled
    pico_stdlib
    hardware_i2c
)

target_include_directories(oled PUBLIC ${CMAKE_CURRENT_LIST_DIR})
