cmake_minimum_required(VERSION 3.19)
# Pico SDK

set(PICO_SDK_PATH ${CMAKE_CURRENT_SOURCE_DIR}/ext/pico-sdk)
include(pico_sdk_import.cmake)

project(FLIPPERMCE LANGUAGES C CXX ASM)

pico_sdk_init()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_compile_options(-Wall -Wextra -Wconversion)

option(FLIPPERMCE_WITH_GUI   "Build FlipperMCE with GUI support" ON)
option(DEBUG_USB_UART "Activate UART over USB for debugging" OFF)

# variants
include(misc/variants.cmake)

# set proper target name
set(TARGET_NAME)
if (DEBUG_USB_UART)
    string(TOLOWER "${VARIANT}-debug" TARGET_NAME PARENT_SCOPE)
else()
    string(TOLOWER "${VARIANT}" TARGET_NAME PARENT_SCOPE)
endif()

message(STATUS "Building ${TARGET_NAME}")


# Add all subdirectories for sub-targets
add_subdirectory(src/version)

add_subdirectory(src/gc)

add_subdirectory(src/psram)
add_subdirectory(ext/)
add_subdirectory(database)

# FlipperMCE Main Lib
add_executable(${TARGET_NAME}
    src/main.c
    src/gc.c

    src/game_db/game_db.c
    src/wear_leveling/wear_leveling.c
    src/wear_leveling/wear_leveling_rp2040_flash.c

    ext/fnv/hash_64a.c
)

target_link_libraries(${TARGET_NAME}
PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_i2c
    hardware_flash
    gamedb
    flippermce_version
    flippermce_common
    gc_card
    sd_fat
)


target_compile_definitions(
    ${TARGET_NAME} PUBLIC
    PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
)

target_include_directories(${TARGET_NAME} PUBLIC
    ext/fnv
)

if(NOT FLIPPERMCE)
    add_dependencies(${TARGET_NAME} gamedb)
endif()
set_target_properties(${TARGET_NAME} PROPERTIES PICO_TARGET_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/memmap_custom.ld)

# Common Lib

add_library(flippermce_common STATIC
                ${CMAKE_CURRENT_SOURCE_DIR}/src/util.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/debug.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/input.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/settings.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/bigmem.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/card_config.c)

target_include_directories(flippermce_common
                PUBLIC
                    ${CMAKE_CURRENT_SOURCE_DIR}/src
                PRIVATE )

target_link_libraries(flippermce_common
                    PRIVATE
                        sd_fat
                        pico_platform_headers
                        hardware_flash
                        hardware_gpio
                        hardware_watchdog
                        pico_multicore
                        inih)

target_compile_options(flippermce_common
                        PUBLIC
                            -Wall -Wextra
                            -fno-jump-tables)

target_compile_definitions(flippermce_common PUBLIC
                            USE_SPI_ARRAY_TRANSFER=3
                            -DFEAT_PS2_CARDSIZE=1
                            -DFEAT_PS2_MMCE=1)

if (FLIPPERMCE_WITH_GUI)
    target_compile_definitions(flippermce_common PUBLIC "WITH_GUI=1")
    target_sources(flippermce_common PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/gui.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/oled.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/ui_menu.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/ui_theme_mono.c
                ${CMAKE_CURRENT_SOURCE_DIR}/src/splash.c)
    target_link_libraries(flippermce_common
                PUBLIC
                    lvgl::lvgl
                PRIVATE
                    ssd1306)
endif()

if (FLIPPERMCE)
    target_compile_definitions(flippermce_common PUBLIC "FLIPPER=1")
endif()

set_target_properties(flippermce_common PROPERTIES PICO_TARGET_LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/memmap_custom.ld)

pico_add_extra_outputs(${TARGET_NAME})
set_property(TARGET ${TARGET_NAME} APPEND_STRING PROPERTY LINK_FLAGS "-Wl,--print-memory-usage")



if(DEBUG_USB_UART)
    set(DEBUG_STARTUP_DELAY 0 CACHE STRING "Startup Delay for Debug Output")
    if (NOT ${DEBUG_STARTUP_DELAY} MATCHES "[0-9]+")
        message(FATAL_ERROR "Misconfigured Debug Startup Delay: ${DEBUG_STARTUP_DELAY}")
    endif()
    target_compile_definitions(flippermce_common PUBLIC -DDEBUG_USB_UART -DMMCE_DEBUG -DPICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=${DEBUG_STARTUP_DELAY}*1000)
    pico_enable_stdio_usb(${TARGET_NAME} 1)
else()
    pico_enable_stdio_usb(${TARGET_NAME} 0)
endif()
