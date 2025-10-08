#include "gc_mmceman_block_commands.h"
#include "pico/platform.h"
#include "sd.h"
#include "pico/critical_section.h"
#include <debug.h>
#include <stdint.h>
#include <string.h>

// Size of each block for SD card operations (must match GameCube memory card spec)
#define SD_BLOCK_SIZE 512

// Static buffers that will be pointed to
static volatile uint8_t sd_buffers[2][SD_BLOCK_SIZE];
static volatile uint8_t sd_write_buffer[SD_BLOCK_SIZE];

typedef struct sd_op_Tag {
    uint8_t* buffer;      // Pointer to double-buffered data array
    uint32_t block_num;   // Current block number being processed
    volatile int result;  // Operation result: 0=pending, 1=success, -1=failure
    volatile int request; // Operation state: 0=idle, 1=read pending, 2=write pending (future)
} sd_op_t;

typedef struct sd_write_op_Tag {
    uint32_t start_block;   // Current block number being processed
    uint32_t block_count;   // Number of blocks to write
    uint32_t blocks_written;  // Number of blocks written so far
    volatile int result;  // Operation result: 0=pending, 1=success, -1=failure
    volatile int request; // Operation state: 0=idle, 1=read pending, 2=write pending (future)
} sd_write_op_t;

static volatile uint32_t read_sectors_remaining = 0;
static sd_write_op_t sd_write_op;
static sd_op_t sd_read_ops[2];

static critical_section_t sd_ops_crit;


static inline void swap_ops(sd_op_t* op1, sd_op_t* op2) {
    sd_op_t temp = *op1;
    *op1 = *op2;
    *op2 = temp;
}

// Helper function to check if a block is ready
static inline bool is_block_ready(const sd_op_t* op, uint32_t block_num) {
    return (op->block_num == block_num && op->result == 1);
}

// Helper function to check if a block is being read
static inline bool is_block_in_progress(const sd_op_t* op, uint32_t block_num) {
    return (op->block_num == block_num && op->request == 1);
}

// Helper function to schedule a new read
static inline void schedule_read(sd_op_t* op, uint32_t block_num) {
    op->block_num = block_num;
    op->result = 0;
    op->request = 1;
}

// Helper function to clear an operation
static inline void clear_op(sd_op_t* op) {
    op->request = 0;
    op->result = 0;
}

// ------ Core 1: Request Handler ------
// Initiates a read request for multiple sectors with read-ahead optimization
// Note: This must only be called from Core 1 as it manages request state
// Note: No error handling for critical section failures - system assumed stable
void __time_critical_func(gc_mmceman_block_request_read_sector)(uint32_t sector, uint32_t count) {
    if (count == 0) return;

    critical_section_enter_blocking(&sd_ops_crit);
    read_sectors_remaining = count;

    // Check if block is already in position 0
    if (is_block_ready(&sd_read_ops[0], sector)
        || is_block_in_progress(&sd_read_ops[0], sector)) {
        // Block is being read or ready in position 0

        // Schedule read ahead only if we have more sectors to read
        if (count - 1  > 0) {
            if (!is_block_ready(&sd_read_ops[1], sector + 1)
                && !is_block_in_progress(&sd_read_ops[1], sector + 1)) {
            schedule_read(&sd_read_ops[1], sector + 1);
            }
        } else {
            // Cancel any read-ahead if this was the last sector
            if (sd_read_ops[1].request == 1 && sd_read_ops[1].result == 0) {
                clear_op(&sd_read_ops[1]);
            }
        }
        critical_section_exit(&sd_ops_crit);
        return;
    }

    // Check if requested block is in read ahead position
    if (is_block_ready(&sd_read_ops[1], sector)
        || is_block_in_progress(&sd_read_ops[1], sector)) {
        swap_ops(&sd_read_ops[0], &sd_read_ops[1]);

        // Schedule new read ahead only if we have more sectors to read
        if (count - 1 > 0) {
            schedule_read(&sd_read_ops[1], sector + 1);
        }

        critical_section_exit(&sd_ops_crit);
        return;
    }

    // Block not available in either position, schedule new reads
    schedule_read(&sd_read_ops[0], sector);

    // Schedule read ahead only if we have more than one sector to read
    if (count  > 1) {
        schedule_read(&sd_read_ops[1], sector + 1);
    } else {
        // Cancel any existing read-ahead
        clear_op(&sd_read_ops[1]);
    }

    critical_section_exit(&sd_ops_crit);
}

bool __time_critical_func(gc_mmceman_block_data_ready)(void) {
    bool ready = false;
    critical_section_enter_blocking(&sd_ops_crit);
    ready = (sd_read_ops[0].result == 1);
    critical_section_exit(&sd_ops_crit);
    return ready;
}

void __time_critical_func(gc_mmceman_block_swap_in_next)(void) {
    critical_section_enter_blocking(&sd_ops_crit);
    swap_ops(&sd_read_ops[0], &sd_read_ops[1]);
    critical_section_exit(&sd_ops_crit);
}

void __time_critical_func(gc_mmceman_block_read_data)(uint8_t** buffer) {
    critical_section_enter_blocking(&sd_ops_crit);
    // If data is ready in position 0
    if (sd_read_ops[0].result == 1) {
        *buffer = sd_read_ops[0].buffer;
        read_sectors_remaining--;
        uint32_t next_block = sd_read_ops[0].block_num + 1;

        // If we have read-ahead data ready, move it to position 0
        if (sd_read_ops[1].result == 1) {
            // Check if position 1 contains the block we actually need next
            if (sd_read_ops[1].block_num != next_block) {
                // Read-ahead contains wrong block, discard it and schedule correct one
                schedule_read(&sd_read_ops[1], next_block);
            }
        } else if (sd_read_ops[1].request == 1 && sd_read_ops[1].block_num != next_block) {
            // If there's an in-progress read for the wrong block, cancel it
            clear_op(&sd_read_ops[1]);
            // Schedule read for the correct next block if needed
            if (read_sectors_remaining > 1) {
                schedule_read(&sd_read_ops[1], next_block);
            }
        } else {
            // No read-ahead data ready yet, just mark current buffer as consumed
            // and ensure correct block is scheduled if needed

            if (read_sectors_remaining > 1 && sd_read_ops[1].request == 0) {
                schedule_read(&sd_read_ops[1], next_block);
            }
        }

    }

    critical_section_exit(&sd_ops_crit);
}

void __time_critical_func(gc_mmceman_block_request_write_sector)(uint32_t sector, uint32_t count) {
    if (count == 0) return;

    critical_section_enter_blocking(&sd_ops_crit);
    sd_write_op.start_block = sector;
    sd_write_op.block_count = count;
    sd_write_op.blocks_written = 0;
    sd_write_op.request = 0;
    sd_write_op.result = 0;
    critical_section_exit(&sd_ops_crit);
}

uint8_t* __time_critical_func(gc_mmceman_get_write_block)(void) {
    critical_section_enter_blocking(&sd_ops_crit);
    sd_write_op.request = 0;
    sd_write_op.result = 0;
    critical_section_exit(&sd_ops_crit);

    return sd_write_buffer;
}

void __time_critical_func(gc_mmceman_block_write_data)(void) {
    critical_section_enter_blocking(&sd_ops_crit);
    if (sd_write_op.request == 0 && sd_write_op.result == 0) {
        sd_write_op.result = 0; // Reset result before writing
        sd_write_op.request = 1; // Mark data as ready to be written
    }
    critical_section_exit(&sd_ops_crit);

    while (sd_write_op.result != 1) {
        tight_loop_contents();
    }
}

// ------ Core 0: SD Card Task ------

static void gc_mmceman_block_read_task(void) {
    for (int i = 0; i < 2; i++) {
        bool has_request = false;
        uint8_t* buffer;
        uint32_t block_num;

        critical_section_enter_blocking(&sd_ops_crit);
        if (sd_read_ops[i].request == 1) {
            has_request = true;
            block_num = sd_read_ops[i].block_num;
            buffer = sd_read_ops[i].buffer;
        }
        critical_section_exit(&sd_ops_crit);

        if (has_request) {
            // Execute the read outside the critical section
            bool read_success = sd_read_sector(block_num, buffer);

            critical_section_enter_blocking(&sd_ops_crit);
            // Only update if this is still the same request
            if (sd_read_ops[i].request == 1 && sd_read_ops[i].block_num == block_num) {
                sd_read_ops[i].result = read_success ? 1 : -1;
                sd_read_ops[i].request = 0;
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
        bool write_success = sd_write_sector(block_num, sd_write_buffer);
        DPRINTF("Write sector %u %s\n", block_num, write_success ? "successful": "failed");

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
}

bool gc_mmceman_block_idle(void) {
    bool ret = false;
    critical_section_enter_blocking(&sd_ops_crit);
    ret = (read_sectors_remaining == 0) && (sd_write_op.block_count == sd_write_op.blocks_written);
    critical_section_exit(&sd_ops_crit);
    return ret;
}
