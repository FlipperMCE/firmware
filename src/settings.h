#pragma once

#include <stdbool.h>
#include <stdint.h>

void settings_load_sd(void);
void settings_init(void);


int settings_get_gc_card(void);
int settings_get_gc_channel(void);
int settings_get_gc_boot_channel(void);
uint8_t settings_get_gc_cardsize(void);
int settings_get_gc_variant(void);
void settings_set_gc_card(int x);
void settings_set_gc_channel(int x);
void settings_set_gc_boot_channel(int x);
void settings_set_gc_cardsize(uint8_t size);

bool settings_get_gc_autoboot(void);
void settings_set_gc_autoboot(bool autoboot);
bool settings_get_gc_game_id(void);
void settings_set_gc_game_id(bool enabled);
bool settings_get_gc_encoding(void);
void settings_set_gc_encoding(bool enabled);

#define IDX_MIN 1
#define IDX_BOOT 0
#define CHAN_MIN 1
#define CHAN_MAX 8

uint8_t settings_get_display_timeout(void);
uint8_t settings_get_display_contrast(void);
uint8_t settings_get_display_vcomh(void);
bool    settings_get_display_flipped(void);
bool settings_get_show_info(void);
void settings_set_display_timeout(uint8_t display_timeout);
void settings_set_display_contrast(uint8_t display_contrast);
void settings_set_display_vcomh(uint8_t display_vcomh);
void settings_set_display_flipped(bool flipped);
void settings_set_show_info(bool show);
