#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PIN_MC_CONNECTED PIN_SENSE

#define GC_MMCEMAN_CMD_IDENTIFIER    0x8B

#define MMCEMAN_GET_DEV_ID      0x00
#define MMCEMAN_SET_GAME_ID     0x11
#define MMCEMAN_GET_GAME_NAME   0x12
#define MMCEMAN_SET_GAME_NAME   0x13
#define MMCEMAN_CMD_BLOCK_START_READ    0x20
#define MMCEMAN_CMD_BLOCK_READ          0x21
#define MMCEMAN_CMD_BLOCK_START_WRITE   0x22
#define MMCEMAN_CMD_BLOCK_WRITE         0x23



void gc_memory_card_main(void);
void gc_memory_card_enter(void);
void gc_memory_card_exit(void);
void gc_memory_card_unload(void);
bool gc_memory_card_running(void);