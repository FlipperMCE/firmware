#include "card_emu/gc_mc_data_interface.h"
#include "card_emu/gc_memory_card.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "gc_cardman.h"
#include "debug.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/structs/iobank0.h"

#include "mmceman/gc_mmceman.h"
#include "mmceman/gc_mmceman_block_commands.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "gc_mc_internal.h"
#include "gc_unlock.h"
#include "gc_mc_spi.pio.h"
#include "pico/time.h"

#include <settings.h>
#include <stdbool.h>
#include <stdint.h>

#if LOG_LEVEL_GC_MC == 0
#define log(x...)
#else
#define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_GC_MC, level, fmt, ##x)
#endif


static uint64_t gc_us_startup;

volatile int reset;

typedef struct {
    uint32_t offset;
    uint32_t sm;
} pio_t;

static pio_t cmd_reader, dat_writer, clock_probe;
static uint8_t interrupt_enable = 0;
uint8_t card_state;

static dma_channel_config dma_wait_config, dma_write_config;
static uint8_t _;
static bool req_int = false;


static int memcard_running;
volatile bool gc_card_active;

static volatile int mc_exit_request, mc_exit_response, mc_enter_request, mc_enter_response;

static inline void __time_critical_func(RAM_pio_sm_drain_tx_fifo)(PIO pio, uint sm) {
    uint instr = (pio->sm[sm].shiftctrl & PIO_SM0_SHIFTCTRL_AUTOPULL_BITS) ? pio_encode_out(pio_null, 32) : pio_encode_pull(false, false);
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {
        pio_sm_exec(pio, sm, instr);
    }
}

static void __time_critical_func(reset_pio)(void) {
    pio_set_sm_mask_enabled(pio0, (1 << cmd_reader.sm) | (1 << dat_writer.sm) | (1 << clock_probe.sm), false);
    pio_restart_sm_mask(pio0, (1 << cmd_reader.sm) | (1 << dat_writer.sm) | (1 << clock_probe.sm));

    pio_sm_exec(pio0, cmd_reader.sm, pio_encode_jmp(cmd_reader.offset));
    pio_sm_exec(pio0, dat_writer.sm, pio_encode_jmp(dat_writer.offset));
    pio_sm_exec(pio0, clock_probe.sm, pio_encode_jmp(clock_probe.offset));

    pio_sm_clear_fifos(pio0, cmd_reader.sm);
//    pio_sm_clear_fifos(pio0, dat_writer.sm);
    RAM_pio_sm_drain_tx_fifo(pio0, dat_writer.sm);
    pio_enable_sm_mask_in_sync(pio0, (1 << cmd_reader.sm) | (1 << dat_writer.sm) | (1 << clock_probe.sm) );

    reset = 1;
}

static void __time_critical_func(init_pio)(void) {

    /* Set all pins as floating inputs */
    gpio_set_dir(PIN_GC_SEL, 0);
    gpio_set_dir(PIN_GC_CLK, 0);
    gpio_set_dir(PIN_GC_DI, 0);
    gpio_set_dir(PIN_GC_DO, 0);
    gpio_disable_pulls(PIN_GC_SEL);
    gpio_disable_pulls(PIN_GC_CLK);
    gpio_disable_pulls(PIN_GC_DI);
    gpio_disable_pulls(PIN_GC_DO);

    cmd_reader.offset = pio_add_program(pio0, &cmd_reader_program);
    cmd_reader.sm = pio_claim_unused_sm(pio0, true);

    dat_writer.offset = pio_add_program(pio0, &dat_writer_program);
    dat_writer.sm = pio_claim_unused_sm(pio0, true);

    clock_probe.offset = pio_add_program(pio0, &clock_probe_program);
    clock_probe.sm = pio_claim_unused_sm(pio0, true);

    cmd_reader_program_init(pio0, cmd_reader.sm, cmd_reader.offset);
    dat_writer_program_init(pio0, dat_writer.sm, dat_writer.offset);
    clock_probe_program_init(pio0, clock_probe.sm, clock_probe.offset);

    gpio_init(PIN_GC_INT);
    gpio_init(PIN_MC_CONNECTED);
    gpio_set_dir(PIN_GC_INT, GPIO_OUT);
    gpio_set_dir(PIN_MC_CONNECTED, GPIO_OUT);

    gpio_put(PIN_GC_INT, 1);
    gpio_put(PIN_MC_CONNECTED, 0);


    dma_wait_config = dma_channel_get_default_config(DMA_WAIT_CHAN);
    channel_config_set_read_increment(&dma_wait_config, false);
    channel_config_set_write_increment(&dma_wait_config, false); // Changed to false to write to same location
    channel_config_set_transfer_data_size(&dma_wait_config, DMA_SIZE_8);
    channel_config_set_dreq(&dma_wait_config, pio_get_dreq(pio0, cmd_reader.sm, false));
    dma_channel_configure(
        DMA_WAIT_CHAN,                 // Channel to be configured
        &dma_wait_config,         // The configuration we just created
        &_,            // Single byte destination address
        &pio0->rxf[cmd_reader.sm],  // Source address
        GC_MC_LATENCY_CYCLES,                    // Number of transfers
        false                   // Start immediately
    );

    dma_write_config = dma_channel_get_default_config(DMA_WRITE_CHAN);
    channel_config_set_read_increment(&dma_write_config, false);
    channel_config_set_write_increment(&dma_write_config, true); // Changed to false to write to same location
    channel_config_set_transfer_data_size(&dma_write_config, DMA_SIZE_8);
    channel_config_set_dreq(&dma_write_config, pio_get_dreq(pio0, cmd_reader.sm, false));
    dma_channel_configure(
        DMA_WRITE_CHAN,                 // Channel to be configured
        &dma_write_config,         // The configuration we just created
        &_,            // Single byte destination address
        &pio0->rxf[cmd_reader.sm],  // Source address
        128,                    // Number of transfers
        false                   // Start immediately
    );
}

static void __time_critical_func(card_deselected)(uint gpio, uint32_t event_mask) {
    if (gpio == PIN_GC_SEL && (event_mask & GPIO_IRQ_EDGE_RISE)) {
        gc_card_active = false;
        reset_pio();
    } else if (gpio == PIN_GC_SEL && (event_mask & GPIO_IRQ_EDGE_FALL)) {
        gc_card_active = true;
        gpio_put(PIN_GC_INT, 1);
    }
}

uint8_t __time_critical_func(gc_receive)(uint8_t *cmd) {
    do {
        while (pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm)) {
            if (reset) {
                return RECEIVE_RESET;
            }
        }
        (*cmd) = (pio_sm_get(pio0, cmd_reader.sm) );
        return RECEIVE_OK;
    }
    while (0);
}

uint8_t __time_critical_func(gc_receiveFirst)(uint8_t *cmd) {
    do {
        while (pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm)
                && pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm)
                && pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm)
                && pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm)
                && pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm)
                && 1) {
            if (reset)
                return RECEIVE_RESET;
            if (mc_exit_request)
                return RECEIVE_EXIT;
        }
        (*cmd) = (pio_sm_get(pio0, cmd_reader.sm) );
        return RECEIVE_OK;
    }
    while (0);
}

void __time_critical_func(gc_mc_respond)(uint8_t ch) {
    pio_sm_put_blocking(pio0, dat_writer.sm, ch << 24);
}


static uint8_t mc_probe_id[2] = {
    0x00, 0x00
};

static void mc_generateId(void) {
    uint32_t size = ((gc_cardman_get_card_size() * 8) / (1024 * 1024));
    uint32_t latency = __builtin_ctz(GC_MC_LATENCY_CYCLES) - __builtin_ctz(0x4);
    uint32_t sector_size = __builtin_ctz(GC_MC_SECTOR_SIZE) - __builtin_ctz(0x2000);
    uint32_t value = (size & 0xfc) |
        ((_ROTL(latency << 2, 6))) |
        ((_ROTL(sector_size << 2, 9)));
    mc_probe_id[0] = (value >> 8) & 0xFF;
    mc_probe_id[1] = value & 0xFF;
}

/*
* This function is called when the memory card is probed.
* It is returning essential card stuff:
* - card_size = id & 0xfc
* - sector_size = card_sector_size[_ROTL(id,23)&0x1c / 4]
* - latency = card_latency[_ROTL(id,26)&0x1c / 4];
* - blocks = ((card_size * 32) / 8) / sector_size
*
* Standard Configuration: 0x5A40
* - card_size 64 MBit
* - Latency 128 Clock Cycles
*/
static void __time_critical_func(mc_probe)(void) {
    uint8_t _;
    gc_receiveOrNextCmd(&_);
    gc_mc_respond(0x00); // out byte 3
    gc_mc_respond(0x00); // out byte 4
    gc_mc_respond(mc_probe_id[0]); // out byte 5
    gc_mc_respond(mc_probe_id[1]); // out byte 6
    card_state = 0x01;
}



static void __time_critical_func(gc_mc_read)(void) {
    uint8_t offset[4] = {};

    uint32_t offset_u32 = 0;
    uint32_t test_offset_u32 = 0;
    uint16_t i = 0;

    gc_receiveOrNextCmd(&offset[3]);
    gc_mc_respond(0xFF); // out byte 3
    gc_receiveOrNextCmd(&offset[2]);
    gc_mc_respond(0xFF); // out byte 4
    gc_receiveOrNextCmd(&offset[1]);
    gc_mc_respond(0xFF); // out byte 5
    gc_receiveOrNextCmd(&offset[0]);

    dma_channel_start(DMA_WAIT_CHAN);
    offset_u32 = (offset[3] << 17) | (offset[2] << 9) | (offset[1] << 7) | (offset[0] & 0x7F);
    test_offset_u32 = ((offset[3] << 29) & 0x60000000) | ((offset[2] << 21) & 0x1FE00000) | ((offset[1] << 19) & 0x00180000) | ((offset[0] << 12) & 0x0007F000);

    if ((test_offset_u32 >= 0x7FEC8000)
        && (test_offset_u32 <= 0x7FECF000)) {
        // Cube expects re-unlock
        card_state = 0x01;
        mc_unlock_stage_0(test_offset_u32);
        return;
    }


    // Setup data read
    gc_mc_data_interface_setup_read_page(offset_u32/512U, true);

    volatile gc_mcdi_page_t *page = gc_mc_data_interface_get_page();
    if (page->page_state != PAGE_DATA_AVAILABLE) {
        log(LOG_ERROR, "%s: page %u not available\n", __func__, offset_u32/512U);
        return;
    }
    while (dma_channel_is_busy(DMA_WAIT_CHAN)); // Wait for DMA to complete
    for (i = 0; i < 0x200; i++) {
        gc_mc_respond(page->data[i]);
    }

    //DPRINTF("R: %08x / %i \n", offset_u32, i);
}


static void __time_critical_func(gc_mc_write)(void) {
    uint8_t data[512] = {};
    uint8_t offset[4] = {};
    uint32_t offset_u32 = 0;
    gc_mc_respond(0xFF); // out byte 1
    gc_receiveOrNextCmd(&offset[3]);
    gc_mc_respond(0xFF); // out byte 2
    gc_receiveOrNextCmd(&offset[2]);
    gc_mc_respond(0xFF); // out byte 3
    gc_receiveOrNextCmd(&offset[1]);
    gc_mc_respond(0xFF); // out byte 4
    gc_receiveOrNextCmd(&offset[0]);

    dma_channel_configure(DMA_WRITE_CHAN, &dma_write_config, &data[0], &pio0->rxf[cmd_reader.sm], 128, true);

    offset_u32 = (offset[3] << 17) | (offset[2] << 9) | (offset[1] << 7) | (offset[0] & 0x7F);
    //DPRINTF("W: %08x / %u\n",offset_u32, 128);

    while (dma_channel_is_busy(DMA_WRITE_CHAN)) {}; // Wait for DMA to complete
    gc_mc_data_interface_write_mc(offset_u32, data, 128);


    card_state |= 0x06; // Set card state to 0x06 (write done)

    if (interrupt_enable & 0x01) {
        // Wait 1ms in jpn
        if (gc_cardman_get_card_enc())
            sleep_us(1000);

        gpio_put(PIN_GC_INT, 0);
    }
}


static void __time_critical_func(mc_erase_sector)(void) {
    uint8_t page[2] = {};
    uint32_t offset_u32 = 0;
    gc_receiveOrNextCmd(&page[1]);
    gc_receiveOrNextCmd(&page[0]);

    offset_u32 = ((page[1] << 17) | (page[0] << 9));
    gc_mc_data_interface_erase(offset_u32);
//    DPRINTF("E: %08x\n", offset_u32);

    card_state |= 0x06; // Set card state to 0x06 (write done)

    if (interrupt_enable & 0x01) {
        sleep_us(1000); // Wait for 200us to ensure the card is ready
        gpio_put(PIN_GC_INT, 0);
    }
}


static void __time_critical_func(mc_get_dev_id)(void) {
    gc_mc_respond(0x38); // out byte 5
    gc_mc_respond(0x42); // out byte 5
    gc_mc_respond(0x01); // out byte 5
    gc_mc_respond(0x01); // out byte 5
}

/**
 * Command:  Set disc ID
 * Request:  0x8B 11 aa aa aa aa bb bb cc cc dd dd
 * Response: 0xXX XX XX XX XX XX XX XX XX XX XX XX
 *
 * Clears disc name as a side effect.
*/
static void __time_critical_func(mc_set_game_id)(void) {
    uint8_t id[10] = {};
    for (int i = 0; i < 10; i++) {
        gc_receive(&id[i]);
    }
    gc_mmceman_set_gameid(id);
    log(LOG_INFO, "Set Game ID: %.10s\n", id);
}

static uint8_t name[65] = { 0x00 };
static void __time_critical_func(mc_set_game_name)(void) {
    for (int i = 0; i < 64; i++) {
        gc_receiveOrNextCmd(&name[i]);
        if (name[i] == 0x00) {
            break;
        }
    }
    name[64] = 0x00;
    log(LOG_INFO, "Set Game Name: %s\n", name);
    //DPRINTF("NAME: %s\n", name);
}

static void __time_critical_func(mc_get_game_name)(void) {
    uint8_t i = 0;
    uint8_t _;
    gc_receive(&_);
    while (name[i] != 0x00 && i < 64) {
        gc_mc_respond(name[i++]);
    }
    for (; i < 64; i++) {
        gc_mc_respond(0x00);
    }
    gc_mc_respond(0x00);
    log(LOG_INFO, "Get Game Name: %s\n", name);
}

static void __time_critical_func(mc_block_start_read)(void) {
    uint8_t sector[4] = {};
    uint8_t count[2] = {};
    uint32_t *sec_u32 = (uint32_t *)sector;
    uint16_t *count_u16 = (uint16_t *)count;

    for (int i = 3; i >= 0; i--) {
        gc_receive(&sector[i]);
    }
    for (int i = 1; i >= 0; i--) {
        gc_receive(&count[i]);
    }

    gc_mmceman_block_request_read_sector(*sec_u32, *count_u16);
    interrupt_enable = 0x01;
    log(LOG_INFO, "Block read start: sector=%u count=%u\n", *sec_u32, *count_u16);
    while (!gc_mmceman_block_data_ready()) {
        tight_loop_contents();
    }


    gpio_put(PIN_GC_INT, 0);
    //sleep_us(50);
     /*
    for (uint32_t block_cnt = 0; block_cnt < *count_u32; block_cnt++) {
        gc_mc_respond(0xFF); // out byte 1-2
        gc_mc_respond(0xFF);
        while (!reset) {
        }
        reset = 0;
        gc_card_active = true;
        log(LOG_WARN, "SELECT\n");
        gpio_put(PIN_GC_INT, 1);
        uint32_t i = 0;

        for (i = 0; (i < 512) && (gc_card_active); i++) {
            //gc_mc_respond(buffer[i]);
            gc_mc_respond(i);
        }
        if (i < 512) {
            log(LOG_WARN, "Card deselected during block read at pos %d\n", i);
            break;
        }

        gc_mmceman_block_swap_in_next();
        log(LOG_WARN, "Block read swap done\n");
        while (!gc_mmceman_block_data_ready()) {
            tight_loop_contents();
        }

        log(LOG_WARN, "Block read data %d\n", block_cnt + 1);
        gc_mmceman_block_read_data(&buffer);
        gpio_put(PIN_GC_INT, 0);
    }*/
}

static void __time_critical_func(mc_block_read)(void) {
    uint8_t _;
    uint8_t* buffer;
    gc_mmceman_block_read_data(&buffer);
    gc_receive(&_);

    for (int i = 0; i < 512; i++) {
        gc_mc_respond(buffer[i]);
    }
    log(LOG_INFO, "Block read data sent\n");
    gc_mmceman_block_swap_in_next();

    while (!gc_mmceman_block_data_ready()) {
        if (mc_exit_request) return;
    }

    if (!gc_mmceman_block_read_idle()) {
        gpio_put(PIN_GC_INT, 0);
    }
}

static void __time_critical_func(mc_block_start_write)(void) {
    uint8_t sector[4] = {};
    uint8_t count[4] = {};
    uint32_t *sec_u32 = (uint32_t *)sector;
    uint16_t count_u16 = 0;
    for (int i = 3; i >= 0; i--) {
        gc_receive(&sector[i]);
    }
    gc_receive(&count[0]);
    gc_receive(&count[1]);
    count_u16 = (uint16_t)(((uint16_t)count[0] << 8) | count[1]);
    while (!gc_mmceman_block_write_idle()) {
        tight_loop_contents();
    }
    log(LOG_INFO, "Block write start: sector=%u count=%u\n", *sec_u32, count_u16);
    gc_mmceman_block_request_write_sector(*sec_u32, count_u16);

    gpio_put(PIN_GC_INT, 0);
}

static void __time_critical_func(mc_block_write)(void) {
    uint8_t* buffer = gc_mmceman_get_write_block();
    for (int i = 0; i < 512; i++) {
        gc_receiveOrNextCmd(&buffer[i]);
    }
    log(LOG_INFO, "Received block data for write\n");
    gc_mmceman_block_write_data();
    while (!reset) {
        tight_loop_contents();
    }
    log(LOG_INFO, "Block write done\n");

    gpio_put(PIN_GC_INT, 0);
}

static void __time_critical_func(mc_block_set_accessmode)(void) {
    uint8_t mode = 0x0;
    gc_receive(&mode);
    gc_mmceman_block_set_sd_mode(mode);
    log(LOG_INFO, "Set access mode: %u\n", mode);

    if (mode == 1)
        gpio_put(PIN_GC_INT, 0);
    else
        req_int = true;
}

static void __time_critical_func(mc_mce_cmd)(void) {
    uint8_t cmd;
    uint8_t mode;
    gc_receiveOrNextCmd(&cmd);
    switch (cmd) {
        case MCE_GET_DEV_ID:
            mc_get_dev_id();
            break;
        case MCE_GET_ACCESS_MODE:
            mode = gc_mmceman_block_get_sd_mode() ? 1 : 0;
            gc_receive(&cmd); // buffer byte
            gc_mc_respond(mode);
            log(LOG_INFO, "Get access mode: %u\n", mode);
            break;
        case MCE_SET_ACCESS_MODE:
            mc_block_set_accessmode();
            break;
        case MCE_SET_GAME_ID:
            mc_set_game_id();
            break;
        case MCE_GET_GAME_NAME:
            mc_get_game_name();
            break;
        case MCE_SET_GAME_NAME:
            mc_set_game_name();
            break;
        case MCE_CMD_BLOCK_START_READ:
            mc_block_start_read();
            break;
        case MCE_CMD_BLOCK_READ:
            mc_block_read();
            break;
        case MCE_CMD_BLOCK_START_WRITE:
            mc_block_start_write();
            break;
        case MCE_CMD_BLOCK_WRITE:
            mc_block_write();
            break;
        default:
            DPRINTF("MCE: Unknown command: %02x ", cmd);
            break;
    }
}

static void __time_critical_func(mc_main_loop)(void) {
    card_state = 0x01;
    uint8_t cmd;
    uint8_t res;

    while (1) {
        cmd = 0;
        res = 0;

        while (!reset) {}; // Wait for reset
        gpio_put(PIN_GC_INT, 1);

        reset = 0;

        res = gc_receiveFirst(&cmd);

        if (res != RECEIVE_OK) {
            if (res == RECEIVE_RESET) {
                continue;
            } else if (res == RECEIVE_EXIT) {
                mc_exit_response = 1;
                mc_exit_request = 0;
                return;
            }
        }

        switch (cmd) {
            case GC_MC_PROBE_CMD:
                //gc_mc_respond(0xFF); // <-- this is second byte of the response already
                mc_probe();
                break;
            case GC_MC_READ_CMD:
                if (card_state & 0x40)
                    gc_mc_read();
                else
                    mc_unlock();
                break;
            case GC_MC_INTERRUPT_ENABLE_CMD:
                gc_receive(&interrupt_enable);
                if (interrupt_enable & 0x01) {
                    // Clear interrupt
                    gpio_put(PIN_GC_INT, 1);
                }
                break;
            case GC_MC_GET_CARD_STATE_CMD: // Get card state
                // GC is already transferring second byte - we need to respond with 3rd byte
                gc_mc_respond(card_state);
                break;
            case GC_MC_VENDOR_ID_CMD: // Vendor ID, wii only
                //gc_mc_respond(0xFF); // <-- this is second byte of the response already
                gc_receiveOrNextCmd(&_);
                gc_mc_respond(0x01); // out byte 3
                gc_mc_respond(0x01); // out byte 4
                break;
            case GC_MC_CLEAR_CARD_STATE_CMD: // Clear card state
                card_state &= 0x41;
                break;
            case GC_MCE_CMD_IDENTIFIER:
                mc_mce_cmd();
                break;
            case GC_MC_ERASE_SECTOR_CMD:
                mc_erase_sector();
                break;
            case GC_MC_WRITE_CMD:
                gc_mc_write();
                break;
            case GC_MC_ERASE_CARD_CMD:
                DPRINTF("ERASE CARD ");
                break;
            default:
                //DPRINTF("Unknown command: %02x ", cmd);
                break;
        }
    }
}

static void __no_inline_not_in_flash_func(mc_main)(void) {
    while (1) {
        while (!mc_enter_request) {}
        mc_enter_response = 1;
        memcard_running = 1;

        while (!gc_cardman_is_idle());
        mc_generateId();

        reset_pio();
        gpio_put(PIN_MC_CONNECTED, 1);
        if (req_int) {
            req_int = false;
            gpio_put(PIN_GC_INT, 0);
        }
        mc_main_loop();
        gpio_put(PIN_MC_CONNECTED, 0);
        log(LOG_TRACE, "%s exit\n", __func__);
    }
}

static gpio_irq_callback_t callbacks[NUM_CORES];

static void __time_critical_func(RAM_gpio_acknowledge_irq)(uint gpio, uint32_t events) {
    check_gpio_param(gpio);

    iobank0_hw->intr[gpio / 8] = events << (4 * (gpio % 8));
}

static void __time_critical_func(RAM_gpio_default_irq_handler)(void) {
    uint core = get_core_num();
    gpio_irq_callback_t callback = callbacks[core];
    io_irq_ctrl_hw_t *irq_ctrl_base = core ? &iobank0_hw->proc1_irq_ctrl : &iobank0_hw->proc0_irq_ctrl;
    for (uint gpio = 0; gpio < NUM_BANK0_GPIOS; gpio += 8) {
        uint32_t events8 = irq_ctrl_base->ints[gpio >> 3u];
        // note we assume events8 is 0 for non-existent GPIO
        for (uint i = gpio; events8 && i < gpio + 8; i++) {
            uint32_t events = events8 & 0xfu;
            if (events) {
                RAM_gpio_acknowledge_irq(i, events);
                if (callback) {
                    callback(i, events);
                }
            }
            events8 >>= 4;
        }
    }
}

static void my_gpio_set_irq_callback(gpio_irq_callback_t callback) {
    uint core = get_core_num();
    if (callbacks[core]) {
        if (!callback) {
            irq_remove_handler(IO_IRQ_BANK0, RAM_gpio_default_irq_handler);
        }
        callbacks[core] = callback;
    } else if (callback) {
        callbacks[core] = callback;
        irq_add_shared_handler(IO_IRQ_BANK0, RAM_gpio_default_irq_handler, GPIO_IRQ_CALLBACK_ORDER_PRIORITY);
    }
}

static void my_gpio_set_irq_enabled_with_callback(uint gpio, uint32_t events, bool enabled, gpio_irq_callback_t callback) {
    gpio_set_irq_enabled(gpio, events, enabled);
    my_gpio_set_irq_callback(callback);
    if (enabled)
        irq_set_enabled(IO_IRQ_BANK0, true);
}

void gc_memory_card_main(void) {
    multicore_lockout_victim_init();
    init_pio();
    gpio_set_dir(PIN_GC_INT, true);
    gpio_put(PIN_GC_INT, 1);
    gpio_set_drive_strength(PIN_GC_INT, GPIO_DRIVE_STRENGTH_12MA);

    gc_us_startup = time_us_64();
    log(LOG_TRACE, "Secondary core!\n");

    my_gpio_set_irq_enabled_with_callback(PIN_GC_SEL, GPIO_IRQ_EDGE_RISE, 1, card_deselected);
    //my_gpio_set_irq_enabled_with_callback(PIN_GC_SEL, GPIO_IRQ_EDGE_FALL, 1, card_deselected);

    gpio_set_slew_rate(PIN_GC_DO, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(PIN_GC_DO, GPIO_DRIVE_STRENGTH_12MA);

    mc_main();
}


void gc_memory_card_exit(void) {
    uint64_t exit_timeout = time_us_64() + (1000 * 1000);
    if (!memcard_running)
        return;

    mc_exit_request = 1;
    while (!mc_exit_response) {
        if (time_us_64() > exit_timeout) {
            multicore_reset_core1();
            multicore_launch_core1(gc_memory_card_main);
        }
    };
    mc_exit_request = mc_exit_response = 0;
    memcard_running = 0;
    log(LOG_TRACE, "MEMCARD EXIT END!\n");
}

void gc_memory_card_enter(void) {
    if (memcard_running)
        return;

    mc_enter_request = 1;
    while (!mc_enter_response) {}
    mc_enter_request = mc_enter_response = 0;
}

void gc_memory_card_unload(void) {
    pio_remove_program(pio0, &cmd_reader_program, cmd_reader.offset);
    pio_sm_unclaim(pio0, cmd_reader.sm);
    pio_remove_program(pio0, &dat_writer_program, dat_writer.offset);
    pio_sm_unclaim(pio0, dat_writer.sm);
    pio_remove_program(pio0, &clock_probe_program, clock_probe.offset);
    pio_sm_unclaim(pio0, clock_probe.sm);
}

bool gc_memory_card_running(void) {
    return (memcard_running != 0);
}