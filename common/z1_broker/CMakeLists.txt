# Z1 Broker Library
add_library(z1_broker INTERFACE)

target_sources(z1_broker INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/z1_broker.c
)

target_include_directories(z1_broker INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(z1_broker INTERFACE
    pico_stdlib
    hardware_gpio
    z1_onyx_bus
)
