# Z1 Command Layer Library (header-only with inline functions)

add_library(z1_commands INTERFACE)

target_include_directories(z1_commands INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(z1_commands INTERFACE
    pico_stdlib
    z1_onyx_bus
    z1_broker
)
