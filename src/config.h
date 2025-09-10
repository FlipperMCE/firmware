#pragma once

#ifndef PIN_BTN_LEFT
    #define PIN_BTN_LEFT 23
#endif
#ifndef PIN_BTN_RIGHT
    #define PIN_BTN_RIGHT 21
#endif


#define OLED_I2C_SDA 28
#define OLED_I2C_SCL 25
#define OLED_I2C_ADDR 0x3C
#define OLED_I2C_PERIPH i2c0
#define OLED_I2C_CLOCK 400000

#define PSRAM_CS 2
#define PSRAM_CLK 3
#define PSRAM_DAT 4  /* IO0-IO3 must be sequential! */
#define PSRAM_CLKDIV 2

#define SD_BAUD 45 * 1000000

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

#define DEBOUNCE_MS 5

#ifndef MMCE_PRODUCT_ID
#define MMCE_PRODUCT_ID 0x1
#endif

#ifndef MMCE_REVISION
#define MMCE_REVISION 0x1
#endif

#ifndef MMCE_PROTOCOL_VER
#define MMCE_PROTOCOL_VER 0x1
#endif
