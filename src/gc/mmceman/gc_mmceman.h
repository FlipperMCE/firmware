#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MMCEMAN_PING 0x1
#define MMCEMAN_GET_STATUS 0x2
#define MMCEMAN_GET_CARD 0x3
#define MMCEMAN_SET_CARD 0x4
#define MMCEMAN_GET_CHANNEL 0x5
#define MMCEMAN_SET_CHANNEL 0x6
#define MMCEMAN_GET_GAMEID 0x7
#define MMCEMAN_SET_GAMEID 0x8
#define MMCEMAN_RESET 0x9


//TEMP
#define MMCEMAN_SWITCH_BOOTCARD 0x20
#define MMCEMAN_UNMOUNT_BOOTCARD 0x30

#define MMCEMAN_MODE_NUM 0x0
#define MMCEMAN_MODE_NEXT 0x1
#define MMCEMAN_MODE_PREV 0x2

extern void (*mmceman_callback)(void);
extern int mmceman_transfer_stage;

extern volatile bool mmceman_tx_queued;
extern volatile uint8_t mmceman_tx_byte;

extern volatile bool mmceman_op_in_progress;
extern volatile bool mmceman_timeout_detected;
extern volatile bool mmceman_fs_abort_read;



extern volatile uint8_t mmceman_cmd;
extern volatile uint8_t mmceman_mode;
extern volatile uint16_t mmceman_cnum;
extern char mmceman_gameid[251];

void gc_mmceman_task(void);

void gc_mmceman_set_cb(void (*cb)(void));
void gc_mmceman_queue_tx(uint8_t byte);

bool gc_mmceman_set_gameid(const uint8_t* game_id);
const char* gc_mmceman_get_gameid(void);

void gc_mmceman_next_ch(bool delay);
void gc_mmceman_prev_ch(bool delay);
void gc_mmceman_next_idx(bool delay);
void gc_mmceman_prev_idx(bool delay);
