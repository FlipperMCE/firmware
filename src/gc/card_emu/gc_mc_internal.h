#pragma once

#include "../gc_dirty.h"
#include "psram/psram.h"
#include "debug.h"


#include <pico/platform.h>
#include <stdint.h>


#define ERASE_SECTORS 16

#define DMA_WAIT_CHAN 4
#define DMA_WRITE_CHAN 5

#define GC_MC_LATENCY_CYCLES ( 0x100 )
#define GC_MC_SECTOR_SIZE    ( 0x2000 )

#define _ROTL(v,s) \
    (s&31 ? ((uint32_t)v<<s)|((uint32_t)v>>(0x20-s)) : v)

enum { RECEIVE_RESET, RECEIVE_EXIT, RECEIVE_OK };

extern int unlock_stage;
extern uint8_t card_state;

extern uint8_t gc_receive(uint8_t *cmd);
extern uint8_t gc_receiveFirst(uint8_t *cmd);
extern void __time_critical_func(gc_mc_respond)(uint8_t ch);

#define gc_receiveOrNextCmd(cmd)          \
    if (gc_receive(cmd) == RECEIVE_RESET) {\
    DPRINTF("Reset at %s:%u", __func__, __LINE__); \
    return;}
