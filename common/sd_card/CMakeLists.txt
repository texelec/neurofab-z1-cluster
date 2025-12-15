# SD Card Library (Stub)

add_library(sd_card_stub STATIC
    sd_card_stub.c
)

target_include_directories(sd_card_stub PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(sd_card_stub PUBLIC
    pico_stdlib
)
