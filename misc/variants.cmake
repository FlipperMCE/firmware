
# Variants...
set(VARIANT "FlipperMCE" CACHE STRING "Firmware variant to build")
set_property(CACHE VARIANT PROPERTY STRINGS
                    "FlipperMCE"
                    "Other")
message(STATUS "Building for ${VARIANT}")
if (VARIANT STREQUAL "FlipperMCE")
    set(PIN_GC_INT 16)
    set(PIN_GC_SEL 17)
    set(PIN_GC_CLK 18)
    set(PIN_GC_DI 19)
    set(PIN_GC_DO 20)
    add_compile_definitions("UART_TX=8"
                            "UART_RX=9"
                            "UART_PERIPH=uart1"
                            "UART_BAUD=3000000"
                            "SD_PERIPH=SPI1"
                            "SD_MISO=24"
                            "SD_MOSI=27"
                            "SD_SCK=26"
                            "SD_CS=29"
                            "FLASH_OFF_EEPROM=0x1fc000"
                            "MMCE_PRODUCT_ID=0x1"
                            "PIN_SENSE=15"
                            "PIN_BTN_LEFT=21"
                            "PIN_BTN_RIGHT=23"
                            )
    add_compile_definitions(PICO_FLASH_SIZE_BYTES=2097152)
else()
    set(PIN_GC_INT 16)
    set(PIN_GC_SEL 17)
    set(PIN_GC_CLK 18)
    set(PIN_GC_DI 19)
    set(PIN_GC_DO 20)
endif()

