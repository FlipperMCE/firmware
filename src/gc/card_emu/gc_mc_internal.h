#pragma once

#include "../gc_dirty.h"
#include "psram/psram.h"
#include "debug.h"


#include <pico/platform.h>
#include <stdint.h>


#define ERASE_SECTORS 16

extern uint DMA_WAIT_CHAN;
extern uint DMA_WRITE_CHAN;
extern uint DMA_BLOCK_READ_CHAN;

#define GC_MC_LATENCY_CYCLES ( 0x80 )
#define GC_MC_SECTOR_SIZE    ( 0x2000 )


#define MCE_GET_DEV_ID                 0x00
#define MCE_GET_ACCESS_MODE            0x01
#define MCE_SET_ACCESS_MODE            0x02
#define MCE_SET_GAME_ID                0x11
#define MCE_GET_GAME_NAME              0x12
#define MCE_SET_GAME_NAME              0x13
#define MCE_CMD_BLOCK_START_READ       0x20
#define MCE_CMD_BLOCK_READ             0x21
#define MCE_CMD_BLOCK_START_WRITE      0x22
#define MCE_CMD_BLOCK_WRITE            0x23

#define GC_MC_PROBE_CMD                0x00
#define GC_MC_READ_CMD                 0x52
#define GC_MC_INTERRUPT_ENABLE_CMD     0x81
#define GC_MC_GET_CARD_STATE_CMD       0x83
#define GC_MC_VENDOR_ID_CMD            0x85
#define GC_MC_CLEAR_CARD_STATE_CMD     0x89
#define GC_MCE_CMD_IDENTIFIER          0x8B
#define GC_MC_ERASE_SECTOR_CMD         0xF1
#define GC_MC_WRITE_CMD                0xF2
#define GC_MC_ERASE_CARD_CMD           0xF4


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
