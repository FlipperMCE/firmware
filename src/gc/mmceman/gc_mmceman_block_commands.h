#ifndef GC_MMCEMAN_BLOCK_COMMANDS_H
#define GC_MMCEMAN_BLOCK_COMMANDS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Initialize block commands system
// ------ Core 1------
extern void gc_mmceman_block_request_read_sector(uint32_t sector, uint16_t count);
extern bool gc_mmceman_block_data_ready(void);
extern void gc_mmceman_block_swap_in_next(void);
extern void gc_mmceman_block_read_data(uint8_t** buffer);
extern void gc_mmceman_block_request_write_sector(uint32_t sector, uint16_t count);
extern void gc_mmceman_block_write_data(void);
extern uint8_t* gc_mmceman_get_write_block(void);



// ------ Core 0------
extern void gc_mmceman_block_task(void);
extern void gc_mmceman_block_init(void);
extern bool gc_mmceman_block_idle(void);
extern void gc_mmceman_block_finish_transfer(void);

#endif // GC_MMCEMAN_BLOCK_COMMANDS_H