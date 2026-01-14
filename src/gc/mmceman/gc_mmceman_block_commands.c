#include "gc_mmceman_block_commands.h"
#include "mmceman/gc_mmceman.h"
#include "pico/platform.h"
#include "gc_cardman.h"
#include "pico/critical_section.h"
#include <debug.h>
#include <stdint.h>
#include <string.h>
#include "sd.h"

// Size of each block for SD card operations (must match GameCube memory card spec)
#define SD_BLOCK_SIZE 512

// Static buffers that will be pointed to
static uint8_t sd_buffers[2][SD_BLOCK_SIZE];
static volatile uint8_t sd_write_buffer[SD_BLOCK_SIZE];

typedef struct sd_op_Tag {
    uint8_t* buffer;      // Pointer to double-buffered data array
    uint32_t block_num;   // Current block number being processed
    volatile enum { SD_OP_IDLE, SD_OP_READY, SD_OP_REQUESTED, SD_OP_IN_PROGRESS, SD_OP_CONSUMED} state;
} sd_op_t;

typedef struct sd_write_op_Tag {
    uint32_t start_block;   // Current block number being processed
    uint16_t block_count;   // Number of blocks to write
    uint32_t blocks_written;  // Number of blocks written so far
    volatile int result;  // Operation result: 0=pending, 1=success, -1=failure
    volatile int request; // Operation state: 0=idle, 1=read pending, 2=write pending (future)
} sd_write_op_t;

static volatile uint16_t read_sectors_remaining = 0;
static sd_write_op_t sd_write_op;
static sd_op_t sd_read_ops[2];
static volatile sd_op_t* sd_read_front = &sd_read_ops[0];
static volatile sd_op_t* sd_read_back = &sd_read_ops[1];

static critical_section_t sd_ops_crit;
static bool sd_mode = false;

static inline void swap_ops(volatile sd_op_t** op1, volatile sd_op_t** op2) {
    volatile sd_op_t* temp = *op1;
    *op1 = *op2;
    *op2 = temp;
}

// Helper function to check if a block is ready
static inline bool is_block_ready(volatile sd_op_t* op, uint32_t block_num) {
    return (op->block_num == block_num && (op->state == SD_OP_READY));
}

// Helper function to check if a block is being read
static inline bool is_block_in_progress(volatile sd_op_t* op, uint32_t block_num) {
    return (op->block_num == block_num && op->state == SD_OP_IN_PROGRESS);
}

// Helper function to schedule a new read
static inline void schedule_read(volatile sd_op_t* op, uint32_t block_num) {
    op->block_num = block_num;
    op->state = SD_OP_REQUESTED;
}

// Helper function to clear an operation
static inline void clear_op(volatile sd_op_t* op) {
    op->state = SD_OP_IDLE;
}

// ------ Core 1: Request Handler ------
// Initiates a read request for multiple sectors with read-ahead optimization
// Note: This must only be called from Core 1 as it manages request state
// Note: No error handling for critical section failures - system assumed stable
void __time_critical_func(gc_mmceman_block_request_read_sector)(uint32_t sector, uint16_t count) {
    if (count == 0) return;

    memset(&sd_write_op, 0, sizeof(sd_write_op_t));
    critical_section_enter_blocking(&sd_ops_crit);
    read_sectors_remaining = count;

    // Check if block is already in position 0
    if (is_block_ready(sd_read_front, sector)
        || is_block_in_progress(sd_read_front, sector)) {
        // Block is being read or ready in position 0

        // Schedule read ahead only if we have more sectors to read
        if (count - 1  > 0) {
            if (!is_block_ready(sd_read_back, sector + 1)
                && !is_block_in_progress(sd_read_back, sector + 1)) {
            schedule_read(sd_read_back, sector + 1);
            }
        } else {
            // Cancel any read-ahead if this was the last sector
            //if (sd_read_ops[1].request == 1 && sd_read_ops[1].result == 0) {
            if (sd_read_back->state == SD_OP_REQUESTED || sd_read_back->state == SD_OP_IN_PROGRESS) {
                clear_op(sd_read_back);
            }
        }
        critical_section_exit(&sd_ops_crit);
        return;
    }

    // Check if requested block is in read ahead position
    if (is_block_ready(sd_read_back, sector)
        || is_block_in_progress(sd_read_back, sector)) {
        swap_ops(&sd_read_front, &sd_read_back);

        // Schedule new read ahead only if we have more sectors to read
        if (count - 1 > 0) {
            schedule_read(sd_read_back, sector + 1);
        }

        critical_section_exit(&sd_ops_crit);
        return;
    }

    // Block not available in either position, schedule new reads
    schedule_read(sd_read_front, sector);

    // Schedule read ahead only if we have more than one sector to read
    if (count  > 1) {
        schedule_read(sd_read_back, sector + 1);
    } else {
        // Cancel any existing read-ahead
        clear_op(sd_read_back);
    }

    critical_section_exit(&sd_ops_crit);
}

bool __time_critical_func(gc_mmceman_block_data_ready)(void) {
    bool ready = false;
    critical_section_enter_blocking(&sd_ops_crit);
    ready = (sd_read_front->state == SD_OP_READY);
    critical_section_exit(&sd_ops_crit);
    return ready;
}

void __time_critical_func(gc_mmceman_block_swap_in_next)(void) {
    critical_section_enter_blocking(&sd_ops_crit);
    swap_ops(&sd_read_front, &sd_read_back);
    if ((read_sectors_remaining > 1)) {
        schedule_read(sd_read_back, sd_read_front->block_num + 1);
    } else {
        clear_op(sd_read_back);
    }
    critical_section_exit(&sd_ops_crit);
}

void __time_critical_func(gc_mmceman_block_read_data)(uint8_t** buffer) {
    // If data is ready in position 0
    if (sd_read_front->state == SD_OP_READY) {
        uint32_t next_block = sd_read_front->block_num + 1;
        *buffer = sd_read_front->buffer;
        read_sectors_remaining--;
        sd_read_front->state = SD_OP_CONSUMED;

        if (read_sectors_remaining > 0) {
            if (sd_read_back->block_num != next_block) {
                critical_section_enter_blocking(&sd_ops_crit);
                if (sd_read_back->state == SD_OP_REQUESTED || sd_read_back->state == SD_OP_IN_PROGRESS) {
                    clear_op(sd_read_back);
                }
                // Schedule read for the correct next block if needed
                schedule_read(sd_read_back, next_block);
                critical_section_exit(&sd_ops_crit);
            }
        } else {
            critical_section_enter_blocking(&sd_ops_crit);
            clear_op(sd_read_back);
            critical_section_exit(&sd_ops_crit);
        }
    } else {
        *buffer = NULL;
    }

}

void __time_critical_func(gc_mmceman_block_request_write_sector)(uint32_t sector, uint16_t count) {
    if (count == 0) return;

    critical_section_enter_blocking(&sd_ops_crit);
    read_sectors_remaining = 0;

    sd_write_op.start_block = sector;
    sd_write_op.block_count = count;
    sd_write_op.blocks_written = 0;
    sd_write_op.request = 0;
    sd_write_op.result = 0;
    critical_section_exit(&sd_ops_crit);
}

uint8_t* __time_critical_func(gc_mmceman_get_write_block)(void) {
    //critical_section_enter_blocking(&sd_ops_crit);
    //sd_write_op.request = 0;
    //sd_write_op.result = 0;
    //critical_section_exit(&sd_ops_crit);

    return (uint8_t*)sd_write_buffer;
}

bool __time_critical_func(gc_mmceman_block_get_sd_mode)(void) {
    return sd_mode;
}

void gc_mmceman_block_set_sd_mode(bool mode) {
    if (mode != sd_mode) {
        sd_mode = mode;
        mmceman_cmd = MMCEMAN_CMDS_SET_ACCESS_MODE;
    }
    if (mode) {
        while (!gc_cardman_is_sd_mode())
            tight_loop_contents();

    }
}


bool gc_mmceman_block_read_idle(void)
{
    bool ret = false;
    critical_section_enter_blocking(&sd_ops_crit);
    ret = (read_sectors_remaining == 0);
    critical_section_exit(&sd_ops_crit);
    return ret;
}

bool gc_mmceman_block_write_idle(void)
{
    bool ret = false;
    critical_section_enter_blocking(&sd_ops_crit);
    ret = (sd_write_op.block_count == sd_write_op.blocks_written);
    critical_section_exit(&sd_ops_crit);
    return ret;
}

void __time_critical_func(gc_mmceman_block_write_data)(void) {
    critical_section_enter_blocking(&sd_ops_crit);
    if (sd_write_op.block_count > sd_write_op.blocks_written) {
        if (sd_write_op.request == 0 && sd_write_op.result == 0) {
            sd_write_op.result = 0; // Reset result before writing
            sd_write_op.request = 1; // Mark data as ready to be written
        }
    } else {
        critical_section_exit(&sd_ops_crit);
        return;
    }
    critical_section_exit(&sd_ops_crit);

    while (sd_write_op.result != 1) {
        tight_loop_contents();
    }
    critical_section_enter_blocking(&sd_ops_crit);
    sd_write_op.result = 0; // Reset result before writing
    sd_write_op.request = 0; // Mark data as cleared
    critical_section_exit(&sd_ops_crit);
}

// ------ Core 0: SD Card Task ------

static void gc_mmceman_block_read_task(void) {
    for (int i = 0; i < 2; i++) {
        bool has_request = false;
        uint8_t* buffer;
        uint32_t block_num;

        critical_section_enter_blocking(&sd_ops_crit);
        if (sd_read_ops[i].state == SD_OP_REQUESTED) {
            has_request = true;
            block_num = sd_read_ops[i].block_num;
            buffer = sd_read_ops[i].buffer;
            sd_read_ops[i].state = SD_OP_IN_PROGRESS;
        }
        critical_section_exit(&sd_ops_crit);

        if (has_request) {
            // Execute the read outside the critical section
            bool read_success = sd_read_sector(block_num, (uint8_t*)buffer);
            //DPRINTF("Read sector %u %s\n", block_num, read_success ? "successful": "failed");

            critical_section_enter_blocking(&sd_ops_crit);
            // Only update if this is still the same request
            if (sd_read_ops[i].state == SD_OP_IN_PROGRESS && sd_read_ops[i].block_num == block_num) {
                sd_read_ops[i].state = read_success ? SD_OP_READY : SD_OP_REQUESTED;
            }
            critical_section_exit(&sd_ops_crit);
        }
    }
}

static void gc_mmceman_block_write_task(void) {
    uint32_t block_num = 0;
    bool write_req = false;
    critical_section_enter_blocking(&sd_ops_crit);
    if (sd_write_op.request == 1) {
        block_num = sd_write_op.start_block + sd_write_op.blocks_written;
        write_req = true;
    }
    critical_section_exit(&sd_ops_crit);
    if (write_req) {
        bool write_success = sd_write_sector(block_num, (uint8_t*)sd_write_buffer);
        //DPRINTF("Write sector %u %s\n", block_num, write_success ? "successful": "failed");

        if (write_success) {
            critical_section_enter_blocking(&sd_ops_crit);
            sd_write_op.blocks_written++;
            sd_write_op.result = 1; // All blocks written successfully
            sd_write_op.request = 0;
            critical_section_exit(&sd_ops_crit);
        } else {
            DPRINTF("Write failed!!\n");
            critical_section_enter_blocking(&sd_ops_crit);
            sd_write_op.result = -1; // Write failed
            sd_write_op.request = 0;
            critical_section_exit(&sd_ops_crit);
        }
    }

}


// Handles actual SD card read operations for both buffers
// Note: This must only be called from Core 0 as it performs SD I/O
// Note: No retry mechanism for failed reads - higher level must handle
// Note: Assumes SD card is initialized and ready
void gc_mmceman_block_task(void) {
    gc_mmceman_block_read_task();
    gc_mmceman_block_write_task();

}

void gc_mmceman_block_init(void) {
    // Note: Critical section initialization assumed to succeed
    // Note: System behavior undefined if initialization fails
    critical_section_init(&sd_ops_crit);

    // Zero all operation state including request/result flags
    memset(sd_read_ops, 0, sizeof(sd_read_ops));
    memset(&sd_write_op, 0, sizeof(sd_write_op));
    // Set up buffer pointers
    sd_read_ops[0].buffer = sd_buffers[0];
    sd_read_ops[1].buffer = sd_buffers[1];

    sd_read_front = &sd_read_ops[0];
    sd_read_back = &sd_read_ops[1];

    sd_mode = false;
}

bool gc_mmceman_block_idle(void) {
    bool ret = false;
    critical_section_enter_blocking(&sd_ops_crit);
    ret = (read_sectors_remaining == 0) && (sd_write_op.block_count == sd_write_op.blocks_written);
    critical_section_exit(&sd_ops_crit);
    return ret;
}

void gc_mmceman_block_finish_transfer(void) {
    while ((read_sectors_remaining > 0)
            || (sd_write_op.blocks_written < sd_write_op.block_count)) {
        gc_mmceman_block_task();
    }
}
