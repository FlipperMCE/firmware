#pragma once

#include <stdint.h>
#include <config.h>
#include <stdbool.h>

extern uint8_t splash_img[(DISPLAY_HEIGHT * DISPLAY_WIDTH / 8) + 8];

extern void splash_init(void);
extern bool splash_load_sd(void);
extern void splash_install(void);
