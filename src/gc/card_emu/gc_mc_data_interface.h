#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define GC_PAGE_SIZE   512


typedef struct {
    size_t page;
    enum {
        PAGE_EMPTY = 0,
        PAGE_READ_REQ = 1,
        PAGE_READ_AHEAD_REQ = 2,
        PAGE_WRITE_REQ = 3,
        PAGE_ERASE_REQ = 4,
        PAGE_DATA_AVAILABLE = 5,
        PAGE_READ_AHEAD_AVAILABLE = 6,
    } page_state;
    uint8_t* data;
} gc_mcdi_page_t;


// Core 1

void gc_mc_data_interface_setup_read_page(uint32_t page, bool readahead, bool wait);
void gc_mc_data_interface_write_mc(uint32_t page, void *buf, uint16_t length);
void gc_mc_data_interface_erase(uint32_t page);
volatile gc_mcdi_page_t* gc_mc_data_interface_get_page(uint32_t page);
void gc_mc_data_interface_commit_write(uint32_t page, uint8_t *buf);
bool gc_mc_data_interface_write_busy(void);
bool gc_mc_data_interface_data_available(void);
void gc_mc_data_interface_wait_for_byte(uint32_t offset);
bool gc_mc_data_interface_delay_required(void);


// Core 0
void gc_mc_data_interface_card_changed(void);
void gc_mc_data_interface_read_core0(uint32_t page, void* buff512);
bool gc_mc_data_interface_write_occured(void);
void gc_mc_data_interface_set_sdmode(bool mode);
bool gc_mc_data_interface_get_sdmode(void);
void gc_mc_data_interface_task(void);
void gc_mc_data_interface_init(void);
void gc_mc_data_interface_flush(void);
