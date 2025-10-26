#include "gc_mmceman.h"

#include <game_db/game_db.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_emu/gc_mc_data_interface.h"
#include "card_emu/gc_memory_card.h"
#include "debug.h"
#include "hardware/timer.h"
#include "mmceman/gc_mmceman_block_commands.h"
#include "pico/time.h"

#if WITH_GUI
#include "gui.h"
#endif

#include "gc/gc_cardman.h"

#include "input.h"

#include "pico/platform.h"

#if LOG_LEVEL_MMCEMAN == 0
#define log(x...)
#else
#define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_MMCEMAN, level, fmt, ##x)
#endif

void (*mmceman_callback)(void);
int mmceman_transfer_stage = 0;


volatile bool mmceman_op_in_progress = false;
volatile bool mmceman_timeout_detected = false;
volatile bool mmceman_fs_abort_read = false;

volatile uint8_t mmceman_cmd;
volatile uint8_t mmceman_mode;
volatile uint16_t mmceman_cnum;

char mmceman_gameid[251] = {0x00};
static uint64_t mmceman_switching_timeout = 0;

void gc_mmceman_task(void) {
    if ((mmceman_cmd != 0) && (!gc_mc_data_interface_write_occured())) {
        switch (mmceman_cmd) {
            case MMCEMAN_CMDS_SET_CARD:
                if (mmceman_mode == MMCEMAN_MODE_NUM) {
                    gc_cardman_set_idx(mmceman_cnum);
                    log(LOG_INFO, "set num idx\n");
                } else if (mmceman_mode == MMCEMAN_MODE_NEXT) {
                    gc_cardman_next_idx();
                    log(LOG_INFO, "set next idx\n");
                } else if (mmceman_mode == MMCEMAN_MODE_PREV) {
                    gc_cardman_prev_idx();
                    log(LOG_INFO, "set prev idx\n");
                }
                break;

            case MMCEMAN_CMDS_SET_CHANNEL:
                if (mmceman_mode == MMCEMAN_MODE_NUM) {
                    gc_cardman_set_channel(mmceman_cnum);
                    log(LOG_INFO, "set num channel\n");
                } else if (mmceman_mode == MMCEMAN_MODE_NEXT) {
                    gc_cardman_next_channel();
                    log(LOG_INFO, "set next channel\n");
                } else if (mmceman_mode == MMCEMAN_MODE_PREV) {
                    gc_cardman_prev_channel();
                    log(LOG_INFO, "set prev channel\n");
                }
                break;

            case MMCEMAN_CMDS_SET_GAMEID:
            {
                const char* game_id;
                const char* region;
                game_db_update_game(mmceman_gameid);
                game_db_get_current_id(&game_id, &region);
                log(LOG_INFO, "%s: game id %s\n", __func__, game_id);
                gc_cardman_set_gameid(game_id, region);
                log(LOG_INFO, "%s: set game id %s\n", __func__, game_id);
                break;
            }

            case MMCEMAN_CMDS_SET_ACCESS_MODE: {
                if (gc_mmceman_block_get_sd_mode()) {
                    gc_mc_data_interface_flush();
                    gc_cardman_set_sd_mode(true);
                    gui_activate_sd_mode();
                } else {
                    gc_cardman_set_sd_mode(false);
                }
                log(LOG_INFO, "%s: set access mode\n", __func__);
                break;
            }

            default: break;
        }

        mmceman_cmd = 0;
    }

    if (gc_cardman_needs_update()
        && (!gc_mmceman_block_get_sd_mode())
        && (mmceman_switching_timeout < time_us_64())
        && !input_is_any_down()) {

        log(LOG_INFO, "%s Switching card now\n", __func__);

        // close old card
        gc_mmceman_block_finish_transfer();
        gc_memory_card_exit();
        gc_mc_data_interface_flush();
        gc_cardman_close();

        sleep_ms(500);
#if WITH_GUI
        gui_do_gc_card_switch();
        gui_request_refresh();
#endif

        // open new card
        gc_cardman_open();
        gc_memory_card_enter();
    }
}

void gc_mmceman_set_cb(void (*cb)(void))
{
    mmceman_callback = cb;
}

bool __time_critical_func(gc_mmceman_set_gameid)(const uint8_t* const game_id) {
    char sanitized_game_id[5] = {0};
    bool ret = false;
    log(LOG_INFO, "Original ID: %s\n", game_id);
    memcpy(sanitized_game_id, game_id, sizeof(sanitized_game_id) - 1);
    if (game_id[0] != 0x00) {
        log(LOG_INFO, "Game ID: %s\n", sanitized_game_id);
        snprintf(mmceman_gameid, sizeof(mmceman_gameid), "%s", sanitized_game_id);
        mmceman_switching_timeout = 0U;
        mmceman_cmd = MMCEMAN_CMDS_SET_GAMEID;
        ret = true;
    }
    return ret;
}

const char* gc_mmceman_get_gameid(void) {
    if (gc_cardman_is_accessible())
        return mmceman_gameid;
    else
        return NULL;
}

void gc_mmceman_next_ch(bool delay) {
    mmceman_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmceman_mode = MMCEMAN_MODE_NEXT;
    mmceman_cmd = MMCEMAN_CMDS_SET_CHANNEL;
}

void gc_mmceman_prev_ch(bool delay) {
    mmceman_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmceman_mode = MMCEMAN_MODE_PREV;
    mmceman_cmd = MMCEMAN_CMDS_SET_CHANNEL;
}

void gc_mmceman_next_idx(bool delay) {
    mmceman_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmceman_mode = MMCEMAN_MODE_NEXT;
    mmceman_cmd = MMCEMAN_CMDS_SET_CARD;
}

void gc_mmceman_prev_idx(bool delay) {
    mmceman_switching_timeout = time_us_64() + (delay ? 1500 * 1000 : 0);
    mmceman_mode = MMCEMAN_MODE_PREV;
    mmceman_cmd = MMCEMAN_CMDS_SET_CARD;
}
