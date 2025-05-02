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
#if WITH_PSRAM
#include "psram.h"
#include "gc_dirty.h"
#endif
#include "gc_mc_internal.h"
#include "gc_cardman.h"

#include "debug.h"

#if LOG_LEVEL_MC_DATA == 0
#define log(x...)
#else
#define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_MC_DATA, level, fmt, ##x)
#endif

#define ERASE_CACHE     2
#define WRITE_CACHE     ( ERASE_CACHE * ERASE_SECTORS )
#define READ_CACHE      3

#define PAGE_CACHE_SIZE ( WRITE_CACHE + ERASE_CACHE + READ_CACHE )
#define MAX_READ_AHEAD  1

#define MAX_TIME_SLICE  ( 5 * 1000 )

#define PAGE_IS_READ(PAGE) ((PAGE->page_state == PAGE_READ_REQ) \
                            || (PAGE->page_state == PAGE_READ_AHEAD_REQ) \
                            || (PAGE->page_state == PAGE_DATA_AVAILABLE) \
                            || (PAGE->page_state == PAGE_READ_AHEAD_AVAILABLE))

static volatile bool dma_in_progress = false;

static volatile gc_mcdi_page_t      writepages[WRITE_CACHE + ERASE_CACHE];
static volatile gc_mcdi_page_t      readpages[READ_CACHE];
static volatile gc_mcdi_page_t*     ops[PAGE_CACHE_SIZE];
static volatile bool                 queue_full = false;
static volatile int                  ops_head = 0;
static volatile int                  ops_tail = 0;
static volatile gc_mcdi_page_t*     curr_read;
static volatile gc_mcdi_page_t*     readahead_read;
static volatile gc_mcdi_page_t*     c0_read;
static volatile bool                 sdmode;
static volatile bool                 write_occured;
static volatile bool                 busy_cycle;
static volatile bool                 delay_reuired;
static critical_section_t            crit;

static volatile uint8_t erase_count = 0;
static volatile uint8_t write_count = 0;
static volatile uint8_t read_count  = 0;

static void gc_mc_data_interface_invalidate_read(void);


static inline void __time_critical_func(push_op)(volatile gc_mcdi_page_t* op) {
    while (queue_full) {log(LOG_INFO, "_");};
    critical_section_enter_blocking(&crit);
    switch (op->page_state) {
        case PAGE_READ_REQ:
        case PAGE_READ_AHEAD_REQ:
            read_count++;
            break;
        case PAGE_WRITE_REQ:
            write_count++;
            break;
        case PAGE_ERASE_REQ:
            erase_count++;
            break;
        default:
        break;
    }
    ops[ops_head] = op;
    ops_head = ( ops_head + 1 ) % PAGE_CACHE_SIZE;
    queue_full = (ops_head == ops_tail);
    delay_reuired = true;
    critical_section_exit(&crit);
}

static inline volatile gc_mcdi_page_t* __time_critical_func(pop_op)(void) {
    critical_section_enter_blocking(&crit);
    volatile gc_mcdi_page_t* ptr = ops[ops_tail];
        switch (ptr->page_state) {
        case PAGE_READ_REQ:
        case PAGE_READ_AHEAD_REQ:
            read_count--;
            break;
        case PAGE_WRITE_REQ:
            write_count--;
            break;
        case PAGE_ERASE_REQ:
            erase_count--;
            break;
        default:
        break;
    }
    ops_tail = (ops_tail + 1) % PAGE_CACHE_SIZE;
    queue_full = false;
    critical_section_exit(&crit);
    return ptr;
}

static inline int __time_critical_func(op_fill_status)(void) {
    return (ops_head - ops_tail + PAGE_CACHE_SIZE) % PAGE_CACHE_SIZE;
}

static inline volatile gc_mcdi_page_t* __time_critical_func(gc_mc_data_interface_find_slot)(bool read) {
    int i = 0;
    volatile gc_mcdi_page_t* page = NULL;
    int size = read ? READ_CACHE : WRITE_CACHE + ERASE_CACHE;
    volatile gc_mcdi_page_t *array = read ? readpages : writepages;
    while (!page) {
        if (array[i].page_state == PAGE_EMPTY) {
            page = &array[i];
            break;
        }
        i++;
        if (i == size)
            log(LOG_INFO, "%s: %s Cache full\n", __func__, read ? "Read" : "Write");
        i = i % size;
    }
    return page;
}

static void gc_mc_data_interface_set_page(volatile gc_mcdi_page_t* page, uint32_t addr, int state) {
    critical_section_enter_blocking(&crit);
    page->page = addr;
    page->page_state = state;
    critical_section_exit(&crit);
}


#if WITH_PSRAM
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
#endif



void __time_critical_func(gc_mc_data_interface_setup_read_page)(uint32_t page, bool readahead, bool wait) {

    if (page * GC_PAGE_SIZE + GC_PAGE_SIZE <= gc_cardman_get_card_size()) {

        volatile gc_mcdi_page_t* page_p = &readpages[get_core_num()];

        log(LOG_TRACE, "%s Waiting page %u - State: %u\n", __func__, page, page_p->page_state);

        critical_section_enter_blocking(&crit);
        page_p->page = page;
        page_p->page_state = PAGE_READ_REQ;
        critical_section_exit(&crit);

        gc_mc_data_interface_start_dma(page_p);

    } else {
        log(LOG_WARN, "%s Addr out of bounds: %u\n", __func__, page);
    }
}

volatile gc_mcdi_page_t* __time_critical_func(gc_mc_data_interface_get_page)(uint32_t page) {
    volatile gc_mcdi_page_t* ret = NULL;

    if (sdmode) {
        log(LOG_INFO, "%s page %u\n", __func__, page);
        if (get_core_num() == 0) {
            if (c0_read->page_state == PAGE_EMPTY || c0_read->page != page) {
                log(LOG_WARN, "%s miss %u\n", __func__, page);
                gc_mc_data_interface_setup_read_page(page, false, true);
            }
            ret = c0_read;
        } else {
            if (curr_read->page_state == PAGE_EMPTY || curr_read->page != page) {
                log(LOG_WARN, "%s miss %u\n", __func__, page);
                gc_mc_data_interface_setup_read_page(page, true, true);
            }
            ret = curr_read;

            if (ret && (ret->page_state == PAGE_READ_AHEAD_AVAILABLE)) {
                ret->page_state = PAGE_DATA_AVAILABLE;
            }
        }
    } else {
#if WITH_PSRAM
        ret = &readpages[get_core_num()];
        //if (!gc_cardman_is_sector_available(ret->page)) {
        //    gc_cardman_set_priority_sector(ret->page);
        //    while (!gc_cardman_is_sector_available(ret->page)) {} // wait for core 0 to load the sector into PSRAM
        //    gc_mc_data_interface_start_dma(ret);
        //}
#endif
    }
    return ret;
}

void __time_critical_func(gc_mc_data_interface_write_mc)(uint32_t addr, void *buf, uint16_t length) {
    if (addr <= gc_cardman_get_card_size()) {
        log(LOG_TRACE, "%s page %u\n", __func__, addr);
        gc_mc_data_interface_invalidate_read();

        psram_wait_for_dma();
        gc_dirty_lockout_renew();
        gc_dirty_lock();
        psram_write_dma(addr, buf, length, NULL);
        gc_cardman_mark_sector_available(addr/GC_PAGE_SIZE);
        psram_wait_for_dma();
        gc_dirty_mark(addr/GC_PAGE_SIZE);
        gc_dirty_unlock();
        write_occured = true;

    }
}

bool __time_critical_func(gc_mc_data_interface_write_busy)(void) {
    if (sdmode)
        return (write_count == WRITE_CACHE);
    else
        return false;
}

bool __time_critical_func(gc_mc_data_interface_delay_required)(void) {
    return delay_reuired;
}

void gc_mc_data_interface_flush(void) {
    while ((sdmode && (op_fill_status() > 0))
    #ifdef WITH_PSRAM
        || gc_dirty_activity > 0
    #endif
    ) {
        gc_mc_data_interface_task();
    }
}

void __time_critical_func(gc_mc_data_interface_erase)(uint32_t page) {
    if ((page + ERASE_SECTORS) * GC_PAGE_SIZE <= gc_cardman_get_card_size()) {
        log(LOG_TRACE, "%s page %u\n", __func__, page);

        gc_mc_data_interface_invalidate_read();

        uint8_t erasebuff[GC_PAGE_SIZE] = { 0 };
        memset(erasebuff, 0xFF, GC_PAGE_SIZE);
        uint32_t page_base = page;
        gc_dirty_lockout_renew();
        gc_dirty_lock();
        for (int i = 0; i < ERASE_SECTORS; ++i) {
            psram_write_dma(page_base + (i * GC_PAGE_SIZE), erasebuff, GC_PAGE_SIZE, NULL);
            psram_wait_for_dma();
            gc_cardman_mark_sector_available(page_base + (i * GC_PAGE_SIZE));
            gc_dirty_mark(page_base + (i * GC_PAGE_SIZE));
        }
        gc_dirty_unlock();
    }
}

bool __time_critical_func(gc_mc_data_interface_data_available)(void) {
    if (sdmode) {
        return (curr_read->page_state == PAGE_DATA_AVAILABLE) || (curr_read->page_state == PAGE_READ_AHEAD_AVAILABLE);
    } else {
        return true;
    }
}


inline void __time_critical_func(gc_mc_data_interface_wait_for_byte)(uint32_t offset) {
#if WITH_PSRAM
    if (!sdmode) {
        if (offset <= GC_PAGE_SIZE)
            while (dma_in_progress && (psram_read_dma_remaining() >= (GC_PAGE_SIZE - offset))) {};
    } else
#endif
    {
        if (get_core_num() != 0) {
            if (curr_read == NULL) {
                log(LOG_ERROR, "%s: No read was set up : %u\n", __func__, offset);
            } else if (!PAGE_IS_READ(curr_read)) {
                log(LOG_ERROR, "%s: Not read page for offs: %u state: %u\n", __func__, offset, curr_read->page_state);
            } else {
                while ((curr_read->page_state != PAGE_DATA_AVAILABLE) && (curr_read->page_state != PAGE_READ_AHEAD_AVAILABLE) ) {/*log(LOG_WARN, "%s:WC:%u,EC:%u\n", __func__,write_count, erase_count);*/sleep_us(50); /*log(LOG_INFO, "-\n");*/}
            }
        }
    }
}

static void __time_critical_func (gc_mc_data_interface_invalidate_read)(void) {
    if (sdmode) {
        for (int i = 0; i < 2; i++)
        {
            if (((readpages[i].page_state == PAGE_DATA_AVAILABLE)
                || (readpages[i].page_state == PAGE_READ_AHEAD_AVAILABLE)))
            {
                critical_section_enter_blocking(&crit);
                readpages[i].page = 0;
                readpages[i].page_state = PAGE_EMPTY;
                critical_section_exit(&crit);
                log(LOG_INFO, "%s Invalidated read\n", __func__);
            }
        }
        log(LOG_TRACE, "%s inv done \n", __func__);

    } else {
        critical_section_enter_blocking(&crit);
        readpages[get_core_num()].page = 0;
        readpages[get_core_num()].page_state = PAGE_EMPTY;
        critical_section_exit(&crit);
    }

}

// Core 0
void gc_mc_data_interface_card_changed(void) {
    for(int i = 0; i < READ_CACHE; i++) {
        readpages[i].page_state = PAGE_EMPTY;
        readpages[i].page = 0;
        readpages[i].data = &cache[i * GC_PAGE_SIZE];
    }
    for(int i = 0; i < (ERASE_CACHE + WRITE_CACHE); i++) {
        writepages[i].page_state = PAGE_EMPTY;
        writepages[i].page = 0;
        writepages[i].data = &cache[(READ_CACHE * GC_PAGE_SIZE) + (i * GC_PAGE_SIZE)];
    }
    for (int i = 0; i < PAGE_CACHE_SIZE; i++) {
        ops[i] = NULL;
    }

    curr_read = &readpages[0];
    readahead_read = &readpages[1];
    c0_read = &readpages[2];

    read_count = 0;
    write_count = 0;
    erase_count = 0;
    write_occured = false;
    delay_reuired = false;

    log(LOG_INFO, "%s Done\n", __func__);
}

void gc_mc_data_interface_init(void) {
    critical_section_init(&crit);
#if WITH_PSRAM
    gc_dirty_init();
#endif
    gc_mc_data_interface_card_changed();
    sdmode = false;
}

bool gc_mc_data_interface_write_occured(void) {
    return write_occured;
}

void gc_mc_data_interface_set_sdmode(bool mode) {
    sdmode = mode;
}

bool gc_mc_data_interface_get_sdmode(void) {
    return sdmode;
}

void __time_critical_func(gc_mc_data_interface_task)(void) {

    write_occured = false;
    if (sdmode) {
        uint64_t time_start = time_us_64();
        static bool flush_req = false;
        busy_cycle = false;

        while ((op_fill_status() > 0) && ((time_us_64() - time_start) < MAX_TIME_SLICE) ){
            volatile gc_mcdi_page_t* page_p = pop_op();
            busy_cycle = true;
            if (page_p)
                switch(page_p->page_state) {
                    case PAGE_READ_REQ:
                        log(LOG_INFO, "%s Reading page %u\n", __func__, page_p->page);
                        gc_cardman_read_sector(page_p->page, page_p->data);
                        gc_mc_data_interface_set_page(page_p, page_p->page, PAGE_DATA_AVAILABLE);
                        break;
                    case PAGE_READ_AHEAD_REQ:
                        log(LOG_INFO, "%s Reading ahead page %u\n", __func__, page_p->page);
                        gc_cardman_read_sector(page_p->page, page_p->data);
                        gc_mc_data_interface_set_page(page_p, page_p->page, PAGE_READ_AHEAD_AVAILABLE);
                        break;
                    case PAGE_WRITE_REQ:
                        log(LOG_INFO, "%s Writing page %u\n", __func__, page_p->page);
                        write_occured = true;
                        gc_cardman_write_sector(page_p->page, page_p->data);
                        gc_mc_data_interface_set_page(page_p, 0, PAGE_EMPTY);
                        flush_req = true;
                        break;
                    case PAGE_ERASE_REQ:
                        log(LOG_INFO, "%s Erasing page %u\n", __func__, page_p->page);
                        write_occured = true;
                        uint8_t erase_buff[GC_PAGE_SIZE] = { 0x0 };
                        memset((void*)erase_buff, 0xFF, GC_PAGE_SIZE);
                        for (int j = 0; j < ERASE_SECTORS; j++) {
                            gc_cardman_write_sector(page_p->page + j, erase_buff);
                        }
                        critical_section_enter_blocking(&crit);
                        page_p->page = 0;
                        page_p->page_state = PAGE_EMPTY;
                        critical_section_exit(&crit);
                        flush_req = true;
                        break;
                    default:
                        break;
                }
        }
        if (op_fill_status() > 0) {
            log(LOG_INFO, "%u left\n", op_fill_status());
        } else if (flush_req) {
            gc_cardman_flush();
            flush_req = false;
        }
        delay_reuired = op_fill_status() > 0;
    } else {
#if WITH_PSRAM
    gc_dirty_task();
    busy_cycle = dma_in_progress;
#endif
    }
}