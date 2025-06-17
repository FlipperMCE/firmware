#include <stdint.h>
#include "hardware/timer.h"

#include "mmceman/gc_mmceman.h"
#include "pico/multicore.h"
#if WITH_GUI
#include "gui.h"
#include "input.h"
#include "oled.h"
#endif
#include "settings.h"
#include "card_emu/gc_mc_data_interface.h"
#include "card_emu/gc_memory_card.h"
#include "gc_dirty.h"
#include "gc_cardman.h"
#include "debug.h"

#include <stdio.h>

#if LOG_LEVEL_GC_MAIN == 0
#define log(x...)
#else
#define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_GC_MAIN, level, fmt, ##x)
#endif

void gc_init(void) {
    log(LOG_INFO, "starting in GC mode\n");

    multicore_launch_core1(gc_memory_card_main);

    gc_mc_data_interface_init();

    gc_cardman_init();

    log(LOG_INFO, "Starting memory card... ");
    gc_cardman_open();
    gc_memory_card_enter();

    log(LOG_INFO, "DONE! (0 us)\n");
    gui_init();

    gui_do_gc_card_switch();
}

bool gc_task(void) {
    gc_mmceman_task();
    gc_cardman_task();
#if WITH_GUI
    gui_task();
    input_task();
    oled_task();
#endif

    if (gc_cardman_is_idle())
        gc_mc_data_interface_task();

    return true;
}

void gc_deinit(void) {

    gc_memory_card_exit();
    while (gc_mc_data_interface_write_occured())
        gc_mc_data_interface_task();
    multicore_reset_core1();
    gc_cardman_close();
    gc_memory_card_unload();
}