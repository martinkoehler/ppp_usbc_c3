cmake_minimum_required(VERSION 3.16)

set(U8G2_DIR ${CMAKE_CURRENT_LIST_DIR}/../u8g2)
set(U8G2_CSRC ${U8G2_DIR}/csrc)

file(GLOB U8G2_SOURCES
    ${U8G2_CSRC}/u8g2*.c
    ${U8G2_CSRC}/u8x8*.c
)

idf_component_register(
    SRCS ${U8G2_SOURCES}
    INCLUDE_DIRS ${U8G2_CSRC}
)

