#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>


#define GC_CARD_IDX_SPECIAL 0

typedef enum  {
    GC_CM_STATE_NAMED,
    GC_CM_STATE_BOOT,
    GC_CM_STATE_GAMEID,
    GC_CM_STATE_NORMAL
} gc_cardman_state_t;

extern int gc_cardman_fd;

void gc_cardman_init(void);
void gc_cardman_task(void);
int gc_cardman_read_sector(int sector, void *buf512);
int gc_cardman_write_sector(int sector, void *buf512);
bool gc_cardman_is_sector_available(int sector);
void gc_cardman_mark_sector_available(int sector);
void gc_cardman_set_priority_sector(int page_idx);
void gc_cardman_flush(void);
void gc_cardman_open(void);
void gc_cardman_close(void);
int gc_cardman_get_idx(void);
int gc_cardman_get_channel(void);
uint32_t gc_cardman_get_card_size(void);

void gc_cardman_set_channel(uint16_t num);
void gc_cardman_next_channel(void);
void gc_cardman_prev_channel(void);

void gc_cardman_switch_bootcard(void);

void gc_cardman_set_idx(uint16_t num);
void gc_cardman_next_idx(void);
void gc_cardman_prev_idx(void);

typedef void (*cardman_cb_t)(int, bool);

void gc_cardman_set_progress_cb(cardman_cb_t func);
char *gc_cardman_get_progress_text(void);

void gc_cardman_set_gameid(const char* game_id);
const char* gc_cardman_get_folder_name(void);
gc_cardman_state_t gc_cardman_get_state(void);

void gc_cardman_set_variant(int variant);

bool gc_cardman_needs_update(void);
bool gc_cardman_is_accessible(void);
bool gc_cardman_is_idle(void);
