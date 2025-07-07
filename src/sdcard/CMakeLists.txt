add_library(sd_new STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/sd.c
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_config.c)

target_include_directories(sd_new
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(sd_new PRIVATE
    pico_stdlib
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    flippermce_common)

target_include_directories(no-OS-FatFS-SD-SDIO-SPI-RPi-Pico BEFORE INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)