#include "card_emu/gc_mc_data_interface.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "gc_cardman.h"
#include "debug.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

#include "pico/multicore.h"
#include "pico/platform.h"
#include "gc_mc_internal.h"
//#include "mmceman/ps2_mmceman.h"
//#include "mmceman/ps2_mmceman_commands.h"
//#include "mmceman/ps2_mmceman_fs.h"
#include "gc_mc_spi.pio.h"
#include "pico/time.h"

#include <settings.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#if LOG_LEVEL_GC_MC == 0
#define log(x...)
#else
#define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_GC_MC, level, fmt, ##x)
#endif

static uint64_t gc_us_startup;

static volatile int reset;

typedef struct {
    uint32_t offset;
    uint32_t sm;
} pio_t;

static pio_t cmd_reader, dat_writer, clock_probe;
static uint8_t interrupt_enable = 0;
static uint8_t card_state;

#define DMA_WAIT_CHAN 4
static dma_channel_config dma_wait_config;
static uint8_t _;


static int memcard_running;
volatile bool gc_card_active;

static volatile int mc_exit_request, mc_exit_response, mc_enter_request, mc_enter_response;

static inline void __time_critical_func(RAM_pio_sm_drain_tx_fifo)(PIO pio, uint sm) {
    uint instr = (pio->sm[sm].shiftctrl & PIO_SM0_SHIFTCTRL_AUTOPULL_BITS) ? pio_encode_out(pio_null, 32) : pio_encode_pull(false, false);
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) {
        pio_sm_exec(pio, sm, instr);
    }
}

static void __time_critical_func(drain_fifos)(void) {
    pio_sm_clear_fifos(pio0, cmd_reader.sm);
//    pio_sm_clear_fifos(pio0, dat_writer.sm);
    RAM_pio_sm_drain_tx_fifo(pio0, dat_writer.sm);

    reset = 1;
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
        0x00000200 - 0x10,                    // Number of transfers
        false                   // Start immediately
    );

    reset = 1;
}

static void __time_critical_func(init_pio)(void) {

    /* Set all pins as floating inputs */
    gpio_set_dir(PIN_PSX_SEL, 0);
    gpio_set_dir(PIN_PSX_CLK, 0);
    gpio_set_dir(PIN_PSX_CMD, 0);
    gpio_set_dir(PIN_PSX_DAT, 0);
    gpio_disable_pulls(PIN_PSX_SEL);
    gpio_disable_pulls(PIN_PSX_CLK);
    gpio_disable_pulls(PIN_PSX_CMD);
    gpio_disable_pulls(PIN_PSX_DAT);

    cmd_reader.offset = pio_add_program(pio0, &cmd_reader_program);
    cmd_reader.sm = pio_claim_unused_sm(pio0, true);

    dat_writer.offset = pio_add_program(pio0, &dat_writer_program);
    dat_writer.sm = pio_claim_unused_sm(pio0, true);

    clock_probe.offset = pio_add_program(pio0, &clock_probe_program);
    clock_probe.sm = pio_claim_unused_sm(pio0, true);

    cmd_reader_program_init(pio0, cmd_reader.sm, cmd_reader.offset);
    dat_writer_program_init(pio0, dat_writer.sm, dat_writer.offset);
    clock_probe_program_init(pio0, clock_probe.sm, clock_probe.offset);

    gpio_init(PIN_PSX_ACK);
    gpio_set_dir(PIN_PSX_ACK, GPIO_OUT);
    gpio_put(PIN_PSX_ACK, 1);
}

static void __time_critical_func(card_deselected)(uint gpio, uint32_t event_mask) {
    if (gpio == PIN_PSX_SEL && (event_mask & GPIO_IRQ_EDGE_RISE)) {
        gc_card_active = false;
//        drain_fifos();
        reset_pio();
    } else if (gpio == PIN_PSX_SEL && (event_mask & GPIO_IRQ_EDGE_FALL)) {
        gc_card_active = true;
        gpio_put(PIN_PSX_ACK, 1);

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
    gc_receiveOrNextCmd(&_);
    gc_mc_respond(0x00); // out byte 4
    gc_receiveOrNextCmd(&_);
    gc_mc_respond(0x07); // out byte 5 --> should give us 128 cycles delay
    gc_receiveOrNextCmd(&_);
    gc_mc_respond(0x40); // out byte 6
    gc_receiveOrNextCmd(&_);
}



static void __time_critical_func(gc_mc_read)(void) {
    uint8_t offset[4] = {};
    uint8_t _;
    uint32_t offset_u32 = 0;
    uint16_t i = 0;

    gc_receiveOrNextCmd(&offset[3]);
    gc_mc_respond(0xFF); // out byte 3
    gc_receiveOrNextCmd(&offset[2]);
    gc_mc_respond(0xFF); // out byte 4
    gc_receiveOrNextCmd(&offset[1]);
    gc_mc_respond(0xFF); // out byte 5
    gc_receiveOrNextCmd(&offset[0]);

    offset_u32 = (offset[3] << 17) | (offset[2] << 9) | (offset[1] << 7) | (offset[0] & 0x7F);

    if (pio_sm_is_rx_fifo_full(pio0, cmd_reader.sm)) {
        DPRINTF("FIFO full\n");
    }
    dma_channel_start(DMA_WAIT_CHAN);

    // Setup data read
    gc_mc_data_interface_setup_read_page(offset_u32/512U, true, false);

    if (pio_sm_is_rx_fifo_full(pio0, cmd_reader.sm)) {
        DPRINTF("FIFO full\n");
    }

    while (dma_channel_is_busy(DMA_WAIT_CHAN)); // Wait for DMA to complete

    volatile gc_mcdi_page_t *page = gc_mc_data_interface_get_page(offset_u32/512U);
    if (page->page_state != PAGE_DATA_AVAILABLE) {
        log(LOG_ERROR, "%s: page %u not available\n", __func__, offset_u32/512U);
        return;
    }

    if (!pio_sm_is_rx_fifo_empty(pio0, cmd_reader.sm)) {
        DPRINTF("FIFO not empty\n");
    }
    for (i = 0; i < 0x10; i++) {
        gc_receiveOrNextCmd(&_);
    }
    for (i = 0; i < 0x200; i++) {
        gc_mc_respond(page->data[i]);
    }

    //DPRINTF("R: %08x / %i \n", offset_u32, i);
}


static void __time_critical_func(gc_mc_write)(void) {
    uint8_t offset[4] = {};
    uint8_t data[512] = {};
    uint16_t i = 0;
    uint32_t offset_u32 = 0;
    uint8_t ret = RECEIVE_OK;
    gc_mc_respond(0xFF); // out byte 1
    gc_receiveOrNextCmd(&offset[3]);
    gc_mc_respond(0xFF); // out byte 2
    gc_receiveOrNextCmd(&offset[2]);
    gc_mc_respond(0xFF); // out byte 3
    gc_receiveOrNextCmd(&offset[1]);
    gc_mc_respond(0xFF); // out byte 4
    gc_receiveOrNextCmd(&offset[0]);
    gc_mc_respond(0xFF); // out byte 5
    //gc_receiveOrNextCmd(&data[0]);

    offset_u32 = (offset[3] << 17) | (offset[2] << 9) | (offset[1] << 7) | (offset[0] & 0x7F);

    while (ret != RECEIVE_RESET && i < 512) {
        ret = gc_receive(&data[i++]);
    }
    //DPRINTF("W: %08x / %u\n",offset_u32, (i-1));
    gc_mc_data_interface_write_mc(offset_u32, data, 128);
    sleep_us(2);
    gpio_put(PIN_PSX_ACK, 0);
}


static void __time_critical_func(mc_erase_sector)(void) {
    uint8_t page[2] = {};
    uint32_t offset_u32 = 0;
    gc_receiveOrNextCmd(&page[1]);
    gc_receiveOrNextCmd(&page[0]);

    offset_u32 = ((page[1] << 17) | (page[0] << 9));

    gc_mc_data_interface_erase(offset_u32);
    //DPRINTF("E: %08x\n", offset_u32);
    sleep_us(2);
    gpio_put(PIN_PSX_ACK, 0);
}


static void __time_critical_func(mc_get_dev_id)(void) {
    gc_mc_respond(0x38); // out byte 5
    gc_mc_respond(0x42); // out byte 5
    gc_mc_respond(0xFF); // out byte 5
    gc_mc_respond(0xFF); // out byte 5
}

static void __time_critical_func(mc_set_game_id)(void) {
    uint8_t id[10] = {};
    for (int i = 0; i < 10; i++) {
        gc_receiveOrNextCmd(&id[i]);
    }
    /*DPRINTF("ID: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9]);*/
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
    DPRINTF("NAME: %s\n", name);
}

static void __time_critical_func(mc_get_game_name)(void) {
    uint8_t i = 0;
    while (name[i] != 0x00 && i < 64) {
        gc_mc_respond(name[i++]);
    }
    for (; i < 64; i++) {
        gc_mc_respond(0x00);
    }
    gc_mc_respond(0x00);
}

static void __time_critical_func(mc_mce_cmd)(void) {
    uint8_t cmd;
    gc_receiveOrNextCmd(&cmd);
    switch (cmd) {
        case 0x00:
            mc_get_dev_id();
            break;
        case 0x11:
            mc_set_game_id();
            break;
        case 0x12:
            mc_get_game_name();
            break;
        case 0x13:
            mc_set_game_name();
            break;
        default:
            DPRINTF("Unknown command: %02x ", cmd);
            break;
    }
}

static void __time_critical_func(mc_main_loop)(void) {
    card_state = 0x41;
    while (1) {

        uint8_t cmd = 0;
        uint8_t res = 0;
        while (!reset) {};

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
            case 0x00:
                gc_mc_respond(0xFF); // <-- this is second byte of the response already
                mc_probe();
                break;
            case 0x52:
                gc_mc_read();
                break;
            case 0x81:
                gc_receive(&interrupt_enable);
                break;
            case 0x83:
                // GC is already transferring second byte - we need to respond with 3rd byte
                gc_mc_respond(card_state);
                card_state = 0x41;
                break;
            case 0x89:
                card_state = 0x41;
                break;
            case 0x8B:
                mc_mce_cmd();
                break;
            case 0xF1:
                mc_erase_sector();
                break;
            case 0xF2:
                gc_mc_write();
                break;
            case 0xF4:
                DPRINTF("ERASE CARD ");
                break;
            default:
                DPRINTF("Unknown command: %02x ", cmd);
                break;
        }
    }
}

static void __no_inline_not_in_flash_func(mc_main)(void) {
    while (1) {
        while (!mc_enter_request) {}
        mc_enter_response = 1;

        memcard_running = 1;

        reset_pio();
        mc_main_loop();
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
    gpio_set_dir(PIN_PSX_ACK, true);
    gpio_put(PIN_PSX_ACK, 1);

    gc_us_startup = time_us_64();
    log(LOG_TRACE, "Secondary core!\n");

    my_gpio_set_irq_enabled_with_callback(PIN_PSX_SEL, GPIO_IRQ_EDGE_RISE, 1, card_deselected);
    my_gpio_set_irq_enabled_with_callback(PIN_PSX_SEL, GPIO_IRQ_EDGE_FALL, 1, card_deselected);

    gpio_set_slew_rate(PIN_PSX_DAT, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(PIN_PSX_DAT, GPIO_DRIVE_STRENGTH_12MA);

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