
add_library(gc_card STATIC
                ${CMAKE_CURRENT_SOURCE_DIR}/mmceman/gc_mmceman.c
                #${CMAKE_CURRENT_SOURCE_DIR}/mmceman/gc_mmceman_commands.c
                #${CMAKE_CURRENT_SOURCE_DIR}/mmceman/gc_mmceman_debug.c
                #${CMAKE_CURRENT_SOURCE_DIR}/mmceman/gc_mmceman_fs.c
                ${CMAKE_CURRENT_SOURCE_DIR}/card_emu/gc_memory_card.c
                ${CMAKE_CURRENT_SOURCE_DIR}/card_emu/gc_mc_data_interface.c
                ${CMAKE_CURRENT_SOURCE_DIR}/card_emu/gc_unlock.c
                ${CMAKE_CURRENT_SOURCE_DIR}/gc_cardman.c
            )

target_include_directories(gc_card
                            PUBLIC
                                ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gc_card PRIVATE
            sd2psx_common
            pico_stdlib
            pico_multicore
            hardware_sync
            hardware_pio
            hardware_flash
            hardware_dma
            sd_fat
            )

if (SD2PSX_WITH_PSRAM)
    target_link_libraries(gc_card PRIVATE
            psram)
    target_sources(gc_card PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/gc_dirty.c)
    pico_generate_pio_header(gc_card ${CMAKE_CURRENT_LIST_DIR}/../psram/qspi.pio)
endif()


configure_file(${CMAKE_CURRENT_LIST_DIR}/card_emu/gc_mc_spi.pio ${CMAKE_CURRENT_BINARY_DIR}/gc_mc_spi.pio)

pico_generate_pio_header(gc_card ${CMAKE_CURRENT_BINARY_DIR}/gc_mc_spi.pio)
