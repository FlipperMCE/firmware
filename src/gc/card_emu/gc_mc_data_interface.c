#include <sd.h>
#include <settings.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/timer.h"
#include "pico/critical_section.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "gc_mc_data_interface.h"
#include "bigmem.h"

#include "psram.h"
#include "gc_dirty.h"

#include "gc_mc_internal.h"
#include "gc_cardman.h"

#include "debug.h"

#if LOG_LEVEL_MC_DATA == 0
#define log(x...)
#else
#define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_MC_DATA, level, fmt, ##x)
#endif

#define READ_CACHE      3

#define MAX_READ_AHEAD  1

#define MAX_TIME_SLICE  ( 5 * 1000 )

static volatile bool dma_in_progress = false;

static volatile gc_mcdi_page_t      readpages[READ_CACHE];
static volatile bool                 write_occured;
static volatile bool                 busy_cycle;
static critical_section_t            crit;


static void gc_mc_data_interface_invalidate_read(void);


static void __time_critical_func(gc_mc_data_interface_rx_done)() {
    dma_in_progress = false;
    gc_dirty_unlock();
}

void __time_critical_func(gc_mc_data_interface_start_dma)(volatile gc_mcdi_page_t* page_p) {
    gc_dirty_lockout_renew();
    /* the spinlock will be unlocked by the DMA irq once all data is tx'd */
    gc_dirty_lock();
    psram_wait_for_dma();
    dma_in_progress = true;
    page_p->page_state = PAGE_DATA_AVAILABLE;
    psram_read_dma(page_p->page * GC_PAGE_SIZE, page_p->data, GC_PAGE_SIZE, gc_mc_data_interface_rx_done);
    log(LOG_INFO, "%s start dma %zu\n", __func__, page_p->page);
    busy_cycle = true;
}



void __time_critical_func(gc_mc_data_interface_setup_read_page)(uint32_t page, bool wait) {

    if (page * GC_PAGE_SIZE + GC_PAGE_SIZE <= gc_cardman_get_card_size()) {

        volatile gc_mcdi_page_t* page_p = &readpages[get_core_num()];

        log(LOG_TRACE, "%s Waiting page %u - State: %u\n", __func__, page, page_p->page_state);

        critical_section_enter_blocking(&crit);
        page_p->page = page;
        page_p->page_state = PAGE_READ_REQ;
        critical_section_exit(&crit);

        gc_mc_data_interface_start_dma(page_p);

        if (wait)
            psram_wait_for_dma();

    } else {
        log(LOG_WARN, "%s Addr out of bounds: %u\n", __func__, page);
    }
}

volatile gc_mcdi_page_t* __time_critical_func(gc_mc_data_interface_get_page)(void) {

    return &readpages[get_core_num()];
}

void __time_critical_func(gc_mc_data_interface_write_mc)(uint32_t addr, void *buf, uint16_t length) {
    if ((addr + length) <= gc_cardman_get_card_size()) {
        log(LOG_TRACE, "%s addr 0x%x (0x%x)\n", __func__, addr, (addr/GC_PAGE_SIZE));
        gc_mc_data_interface_invalidate_read();

        psram_wait_for_dma();
        gc_dirty_lockout_renew();
        gc_dirty_lock();
        psram_write_dma(addr, buf, length, NULL);
        psram_wait_for_dma();
        gc_dirty_mark(addr/GC_PAGE_SIZE);
        gc_dirty_unlock();
        gc_cardman_mark_segment_available(addr/GC_PAGE_SIZE);
        write_occured = true;

    }
}

void gc_mc_data_interface_flush(void) {
    while (gc_dirty_activity > 0) {
        gc_mc_data_interface_task();
    }
}

void __time_critical_func(gc_mc_data_interface_erase)(uint32_t page) {
    if ((page + ERASE_SECTORS) * GC_PAGE_SIZE <= gc_cardman_get_card_size()) {
        log(LOG_TRACE, "%s page %u\n", __func__, page);

        gc_mc_data_interface_invalidate_read();

        uint8_t erasebuff[GC_PAGE_SIZE] = { 0 };
        memset(erasebuff, 0xFF, GC_PAGE_SIZE);
        gc_dirty_lockout_renew();
        gc_dirty_lock();
        for (int i = 0; i < ERASE_SECTORS; ++i) {
            psram_write_dma(page + (i * GC_PAGE_SIZE), erasebuff, GC_PAGE_SIZE, NULL);
            psram_wait_for_dma();
            gc_cardman_mark_segment_available(page + (i * GC_PAGE_SIZE));
            gc_dirty_mark(page/GC_PAGE_SIZE + i);
        }
        gc_dirty_unlock();
    }
}

inline void __time_critical_func(gc_mc_data_interface_wait_for_byte)(uint32_t offset) {
    if (offset <= GC_PAGE_SIZE)
        while (dma_in_progress && (psram_read_dma_remaining() >= (GC_PAGE_SIZE - offset))) {};
}

static void __time_critical_func (gc_mc_data_interface_invalidate_read)(void) {

    critical_section_enter_blocking(&crit);
    readpages[get_core_num()].page = 0;
    readpages[get_core_num()].page_state = PAGE_EMPTY;
    critical_section_exit(&crit);

}

// Core 0
void gc_mc_data_interface_card_changed(void) {
    for(int i = 0; i < READ_CACHE; i++) {
        readpages[i].page_state = PAGE_EMPTY;
        readpages[i].page = 0;
        readpages[i].data = &cache[i * GC_PAGE_SIZE];
    }


    write_occured = false;

    log(LOG_INFO, "%s Done\n", __func__);
}

void gc_mc_data_interface_init(void) {
    critical_section_init(&crit);

    gc_dirty_init();

    gc_mc_data_interface_card_changed();
}

bool gc_mc_data_interface_write_occured(void) {
    return write_occured;
}

void __time_critical_func(gc_mc_data_interface_task)(void) {
    write_occured = false;

    gc_dirty_task();
    busy_cycle = dma_in_progress;
}