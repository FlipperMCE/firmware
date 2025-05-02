#include "gc_cardman.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_emu/gc_mc_data_interface.h"

#include "debug.h"
#include "game_db/game_db.h"
#include "hardware/timer.h"

#include "pico/multicore.h"
#include "pico/platform.h"
#if WITH_PSRAM
    #include "gc_dirty.h"
    #include "psram/psram.h"
#endif
#include "sd.h"
#include "settings.h"
#include "util.h"
#include "card_config.h"

#if LOG_LEVEL_GC_CM == 0
    #define log(x...)
#else
    #define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_GC_CM, level, fmt, ##x)
#endif

#define BLOCK_SIZE   (512)

#define CARD_HOME_GC        "MemoryCards/GC"
#define CARD_HOME_LENGTH    (17)

static int sector_count = -1;

#if WITH_PSRAM
#define SECTOR_COUNT_4MB (8*1024*1024 / BLOCK_SIZE)
uint8_t gc_available_sectors[SECTOR_COUNT_4MB / 8];  // bitmap
#define PSRAM_AVAILABLE true
#else
#define PSRAM_AVAILABLE false
#endif
static uint8_t flushbuf[BLOCK_SIZE];
int gc_cardman_fd = -1;

static int current_read_sector = 0, priority_sector = -1;

#define MAX_GAME_NAME_LENGTH (127)
#define MAX_PREFIX_LENGTH    (4)
#define MAX_SLICE_LENGTH     (30 * 1000)

static int card_idx;
static int card_chan;
static bool needs_update;
static uint32_t card_size;
static uint8_t card_enc;
static cardman_cb_t cardman_cb;
static char folder_name[MAX_FOLDER_NAME_LENGTH];
static char cardhome[CARD_HOME_LENGTH];
static uint64_t cardprog_start;
static int cardman_sectors_done;
static uint32_t cardprog_pos;

static gc_cardman_state_t cardman_state;

static enum { CARDMAN_CREATE, CARDMAN_OPEN, CARDMAN_IDLE } cardman_operation;

static bool try_set_boot_card() {
    if (!settings_get_gc_autoboot())
        return false;

    card_idx = GC_CARD_IDX_SPECIAL;
    card_chan = settings_get_gc_boot_channel();
    cardman_state = GC_CM_STATE_BOOT;
    snprintf(folder_name, sizeof(folder_name), "BOOT");
    return true;
}

static void set_default_card() {
    card_idx = settings_get_gc_card();
    card_chan = settings_get_gc_channel();
    cardman_state = GC_CM_STATE_NORMAL;
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
}

static bool try_set_game_id_card() {
    if (!settings_get_gc_game_id())
        return false;

    char id[MAX_GAME_ID_LENGTH] = {};

    (void)game_db_get_current_id(id);

    if (!id[0])
        return false;

    card_idx = GC_CARD_IDX_SPECIAL;
    card_chan = CHAN_MIN;
    cardman_state = GC_CM_STATE_GAMEID;
    card_config_get_card_folder(id, folder_name, sizeof(folder_name));
    if (folder_name[0] == 0x00)
        snprintf(folder_name, sizeof(folder_name), "%s", id);

    return true;
}

int gc_cardman_read_sector(int sector, void *buf512) {
    if (gc_cardman_fd < 0)
        return -1;

    if (sd_seek(gc_cardman_fd, sector * BLOCK_SIZE, SEEK_SET) != 0)
        return -1;

    if (sd_read(gc_cardman_fd, buf512, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;

    return 0;
}

static bool try_set_next_named_card() {
    bool ret = false;
    if (cardman_state != GC_CM_STATE_NAMED) {
        ret = try_set_named_card_folder(cardhome, 0, folder_name, sizeof(folder_name));
        if (ret)
            card_idx = 1;
    } else {
        ret = try_set_named_card_folder(cardhome, card_idx, folder_name, sizeof(folder_name));
        if (ret)
            card_idx++;
    }

    if (ret) {
        card_chan = CHAN_MIN;
        cardman_state = GC_CM_STATE_NAMED;
    }

    return ret;
}

static bool try_set_prev_named_card() {
    bool ret = false;
    if (card_idx > 1) {
        ret = try_set_named_card_folder(cardhome, card_idx - 2, folder_name, sizeof(folder_name));
        if (ret) {
            card_idx--;
            card_chan = CHAN_MIN;
            cardman_state = GC_CM_STATE_NAMED;
        }
    }
    return ret;
}

int gc_cardman_write_sector(int sector, void *buf512) {
    if (gc_cardman_fd < 0)
        return -1;

    if (sd_seek(gc_cardman_fd, sector * BLOCK_SIZE, SEEK_SET) != 0)
        return -2;

    if (sd_write(gc_cardman_fd, buf512, BLOCK_SIZE) != BLOCK_SIZE)
        return -3;

    return 0;
}

bool gc_cardman_is_sector_available(int sector) {
#if WITH_PSRAM
    return gc_available_sectors[sector / 8] & (1 << (sector % 8));
#else
    return true;
#endif
}

void gc_cardman_mark_sector_available(int sector) {
#if WITH_PSRAM
    gc_available_sectors[sector / 8] |= (1 << (sector % 8));
#endif
}

void gc_cardman_set_priority_sector(int sector) {
    priority_sector = sector;
}

void gc_cardman_flush(void) {
    if (gc_cardman_fd >= 0)
        sd_flush(gc_cardman_fd);
}

static void ensuredirs(void) {
    char cardpath[CARD_HOME_LENGTH + MAX_FOLDER_NAME_LENGTH + 2];
    snprintf(cardhome, sizeof(cardhome), CARD_HOME_GC);

    snprintf(cardpath, sizeof(cardpath), "%s/%s", cardhome, folder_name);

    sd_mkdir("MemoryCards");
    sd_mkdir(cardhome);
    sd_mkdir(cardpath);

    if (!sd_exists("MemoryCards") || !sd_exists(cardhome) || !sd_exists(cardpath))
        fatal("error creating directories");
}

static void checksum(uint8_t *buff,int32_t len,uint16_t *cs1,uint16_t *cs2)
{
    uint16_t csum = 0;
    uint16_t inv_csum = 0;

    for (int32_t i = 0; i < len; i += 2)
    {
      uint16_t d = swap16(*(uint16_t*)(&buff[i]));
      csum += d;
      inv_csum += (uint16_t)(d ^ 0xffff);
    }

    csum = swap16(csum);
    inv_csum = swap16(inv_csum);

    if (csum == 0xffff)
      csum = 0;
    if (inv_csum == 0xffff)
      inv_csum = 0;
    *cs1 = csum;
    *cs2 = inv_csum;
}

static void genblock(size_t pos, void *vbuf) {
    #define GC_POS_HEADER   0x0000
    #define GC_POS_DIRENT_1 0x4000 - 0x200
    #define GC_POS_DIRENT_2 0x6000 - 0x200
    #define GC_POS_FAT_1    0x6000
    #define GC_POS_FAT_2    0x8000
    #define GC_CARDSIZE_MB  (card_size / (1024 * 1024))
    #define GC_CARDSIZE     (GC_CARDSIZE_MB*8)
    #define GC_FREEBLOCKS   ((((GC_CARDSIZE<<20)>>3)/0x0002000)-5)
    if (pos == GC_POS_HEADER) {
        uint8_t *buf = vbuf;
        memset(buf, 0xFF, 512);
        memset(buf, 0x00, 12); // serial
        buf[12] = 0x00; // time
        buf[13] = 0x00; // time
        buf[14] = 0x00; // time
        buf[15] = 0x00; // time
        buf[16] = 0x00; // time
        buf[17] = 0x00; // time
        buf[18] = 0x00; // time
        buf[19] = 0x00; // time
        buf[20] = 0x00; // sram bias
        buf[21] = 0x00; // sram bias
        buf[22] = 0x00; // sram bias
        buf[23] = 0x00; // sram bias
        buf[24] = 0x00; // lang
        buf[25] = 0x00; // lang
        buf[26] = 0x00; // lang
        buf[27] = 0x01; // lang
        buf[28] = 0x00; // dtv status
        buf[29] = 0x00; // dtv status
        buf[30] = 0x00; // dtv status
        buf[31] = 0x00; // dtv status
        buf[32] = 0x00; // device id
        buf[33] = 0x00; // device id
        buf[34] = GC_CARDSIZE >> 8; // size msb
        buf[35] = GC_CARDSIZE & 0xFF; // size lsb
        buf[36] = 0x00; // encoding
        buf[37] = card_enc; // encoding
        buf[0x200-0x6] = 0x00; // updated
        buf[0x200-0x5] = 0x00; // updated
        checksum(buf, 508, (uint16_t*)&buf[0x200-0x4], (uint16_t*)&buf[0x200-0x2]);

        return;
    } else if ((pos == GC_POS_DIRENT_1) || (pos == GC_POS_DIRENT_2)) {
        uint8_t *buf = vbuf;
        memset(buf, 0xFF, 512);
        buf[0x200-0x1] = 0x00; // 0xFF
        buf[0x200-0x2] = 0x00; // 0xFF
        buf[0x200-0x3] = 0x03; // 0xFF
        buf[0x200-0x4] = 0xF0; // 0xFF
        buf[0x200-0x5] = 0x00; // 0xFF
        buf[0x200-0x6] = 0x00; // 0xFF
        return;
    } else if ((pos == GC_POS_FAT_1) || (pos == GC_POS_FAT_2)) {
        uint8_t *buf = vbuf;
        memset(buf, 0x00, 512);
        buf[4] = 0x00;  // updated msb
        buf[5] = (pos == GC_POS_FAT_1) ? 0 : 1;  // updated lsb
        buf[6] = GC_FREEBLOCKS >> 8;  // freeblocks msb
        buf[7] = GC_FREEBLOCKS & 0xFF;  // freeblocks lsb
        buf[8] = 0x00;  // lastalloc msb
        buf[9] = 0x04;  // lastalloc lsb
        checksum(&buf[4], 508, (uint16_t*)&buf[0], (uint16_t*)&buf[2]);
        uint16_t cs2 = swap16(*(uint16_t*)&buf[2]);
        cs2 -= 0xF00;
        buf[2] = cs2 >> 8; // checksum msb
        buf[3] = cs2 & 0xFF; // checksum lsb
        return;
    } else if (pos > GC_POS_FAT_1 && pos < GC_POS_FAT_2) {
        uint8_t *buf = vbuf;
        memset(buf, 0x00, 512);
        return;
    } else if (pos > GC_POS_FAT_2 && pos < (GC_POS_FAT_2 + 0x2000)) {
        uint8_t *buf = vbuf;
        memset(buf, 0x00, 512);
        return;
    } else {
        uint8_t *buf = vbuf;
        memset(buf, 0xFF, 512);
        return;
    }
}

static int next_sector_to_load() {
    if (priority_sector != -1) {
        if (gc_cardman_is_sector_available(priority_sector))
            priority_sector = -1;
        else
            return priority_sector;
    }

    while (current_read_sector < sector_count) {
        if (!gc_cardman_is_sector_available(current_read_sector))
            return current_read_sector++;
        else
            current_read_sector++;
    }

    return -1;
}

static void gc_cardman_continue(void) {
    if (cardman_operation == CARDMAN_OPEN) {
        uint64_t slice_start = time_us_64();

        log(LOG_TRACE, "%s:%u\n", __func__, __LINE__);
        while ((time_us_64() - slice_start < MAX_SLICE_LENGTH)) {
            log(LOG_TRACE, "Slice!\n");

            gc_dirty_lock();
            int sector_idx = next_sector_to_load();
            if (sector_idx == -1) {
                gc_dirty_unlock();
                cardman_operation = CARDMAN_IDLE;
                uint64_t end = time_us_64();
                log(LOG_INFO, "took = %.2f s; SD read speed = %.2f kB/s\n", (end - cardprog_start) / 1e6,
                    1000000.0 * card_size / (end - cardprog_start) / 1024);
                if (cardman_cb)
                    cardman_cb(100, true);
                break;
            }

            size_t pos = sector_idx * BLOCK_SIZE;
            if (sd_seek(gc_cardman_fd, pos, 0) != 0)
                fatal("cannot read memcard\nseek");

            if (sd_read(gc_cardman_fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                fatal("cannot read memcard\nread %u", pos);

            log(LOG_TRACE, "Writing pos %u\n", pos);
            psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);

            psram_wait_for_dma();

            gc_cardman_mark_sector_available(sector_idx);
            gc_dirty_unlock();

            cardprog_pos = cardman_sectors_done * BLOCK_SIZE;

            if (cardman_cb)
                cardman_cb(100U * (uint64_t)cardprog_pos / (uint64_t)card_size, false);

            cardman_sectors_done++;
        }
        log(LOG_TRACE, "%s:%u\n", __func__, __LINE__);

    } else if (cardman_operation == CARDMAN_CREATE) {
        uint64_t slice_start = time_us_64();
        sd_seek(gc_cardman_fd, cardprog_pos * BLOCK_SIZE, SEEK_SET);
        while ((time_us_64() - slice_start < MAX_SLICE_LENGTH)) {
            cardprog_pos = cardman_sectors_done * BLOCK_SIZE;
            if (cardprog_pos >= card_size) {
                sd_flush(gc_cardman_fd);
                log(LOG_INFO, "OK!\n");

                cardman_operation = CARDMAN_IDLE;
                uint64_t end = time_us_64();

                log(LOG_INFO, "took = %.2f s; SD write speed = %.2f kB/s\n", (end - cardprog_start) / 1e6,
                    1000000.0 * card_size / (end - cardprog_start) / 1024);
                if (cardman_cb)
                    cardman_cb(100, true);

                break;
            }

                gc_dirty_lock();
                psram_wait_for_dma();

                // read back from PSRAM to make sure to retain already rewritten sectors, if any
                psram_read_dma(cardprog_pos, flushbuf, BLOCK_SIZE, NULL);
                psram_wait_for_dma();

                if (sd_write(gc_cardman_fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                    fatal("cannot init memcard");

                gc_dirty_unlock();


            if (cardman_cb)
                cardman_cb(100U * (uint64_t)cardprog_pos / (uint64_t)card_size, cardman_operation == CARDMAN_IDLE);

            cardman_sectors_done++;
        }
        sd_flush(gc_cardman_fd);

    } else if (cardman_cb) {
        cardman_cb(100, true);
    }
}

void gc_cardman_open(void) {
    char path[256];

    needs_update = false;

    sd_init();
    ensuredirs();

    switch (cardman_state) {
        case GC_CM_STATE_BOOT:
            if (card_chan == 1) {
                snprintf(path, sizeof(path), "%s/%s/BootCard-%d.mcd", cardhome, folder_name, card_chan);
                if (!sd_exists(path)) {
                    // before boot card channels, boot card was located at BOOT/BootCard.mcd, for backwards compatibility check if it exists
                    snprintf(path, sizeof(path), "%s/%s/BootCard.mcd", cardhome, folder_name);
                }
                if (!sd_exists(path)) {
                    // go back to BootCard-1.mcd if it doesn't
                    snprintf(path, sizeof(path), "%s/%s/BootCard-%d.mcd", cardhome, folder_name, card_chan);
                }
            } else {
                snprintf(path, sizeof(path), "%s/%s/BootCard-%d.mcd", cardhome, folder_name, card_chan);
            }

            settings_set_gc_boot_channel(card_chan);
            break;
        case GC_CM_STATE_NAMED:
        case GC_CM_STATE_GAMEID: snprintf(path, sizeof(path), "%s/%s/%s-%d.mcd", cardhome, folder_name, folder_name, card_chan); break;
        case GC_CM_STATE_NORMAL:
            snprintf(path, sizeof(path), "%s/%s/%s-%d.mcd", cardhome, folder_name, folder_name, card_chan);

            /* this is ok to do on every boot because it wouldn't update if the value is the same as currently stored */
            settings_set_gc_card(card_idx);
            settings_set_gc_channel(card_chan);
            break;
    }

    log(LOG_INFO, "Switching to card path = %s\n", path);
    gc_mc_data_interface_card_changed();

    if (!sd_exists(path)) {
        card_size = card_config_get_gc_cardsize(folder_name, (cardman_state == GC_CM_STATE_BOOT) ? "BootCard" : folder_name) * 1024 * 1024 / 8;
        if (card_size == 0U) {
            card_size = settings_get_gc_cardsize() * 1024 * 1024 / 8;
        }
        cardman_operation = CARDMAN_CREATE;
        gc_cardman_fd = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);
        cardman_sectors_done = 0;
        cardprog_pos = 0;

        if (gc_cardman_fd < 0)
            fatal("cannot open for creating new card");

        log(LOG_INFO, "create new image at %s... ", path);

        if (cardman_cb)
            cardman_cb(0, false);
        // quickly generate and write an empty card into PSRAM so that it's immediately available, takes about ~0.6s
        for (size_t pos = 0; pos < card_size; pos += BLOCK_SIZE) {
            genblock(pos, flushbuf);

            gc_dirty_lock();
            psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);
            psram_wait_for_dma();
            gc_cardman_mark_sector_available(pos / BLOCK_SIZE);
            gc_dirty_unlock();
        }
        log(LOG_TRACE, "%s created empty PSRAM image... \n", __func__);
        cardprog_start = time_us_64();

    } else {
        gc_cardman_fd = sd_open(path, O_RDWR);
        card_size = sd_filesize(gc_cardman_fd);
        cardman_operation = CARDMAN_OPEN;
        cardprog_pos = 0;
        cardman_sectors_done = 0;

        if (gc_cardman_fd < 0)
            fatal("cannot open card");

        /* read 8 megs of card image */
        log(LOG_INFO, "reading card (%lu KB).... ", (uint32_t)(card_size / 1024));
        cardprog_start = time_us_64();
        if (cardman_cb)
            cardman_cb(0, false);
    }

    sector_count = card_size / BLOCK_SIZE;

    log(LOG_INFO, "Open Finished!\n");
}

void gc_cardman_close(void) {
    if (gc_cardman_fd < 0)
        return;
    gc_cardman_flush();
    sd_close(gc_cardman_fd);
    gc_cardman_fd = -1;
    current_read_sector = 0;
    priority_sector = -1;
#if WITH_PSRAM
    memset(gc_available_sectors, 0, sizeof(gc_available_sectors));
#endif
}

void gc_cardman_set_channel(uint16_t chan_num) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == GC_CM_STATE_BOOT) ? "BootCard" : folder_name);
    if (chan_num != card_chan)
        needs_update = true;
    if (chan_num <= max_chan && chan_num >= CHAN_MIN) {
        card_chan = chan_num;
    }
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
}

void gc_cardman_next_channel(void) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == GC_CM_STATE_BOOT) ? "BootCard" : folder_name);
    card_chan += 1;
    if (card_chan > max_chan)
        card_chan = CHAN_MIN;
    needs_update = true;
}

void gc_cardman_prev_channel(void) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == GC_CM_STATE_BOOT) ? "BootCard" : folder_name);
    card_chan -= 1;
    if (card_chan < CHAN_MIN)
        card_chan = max_chan;
    needs_update = true;
}

//TEMP
void gc_cardman_switch_bootcard(void) {
    if (try_set_boot_card())
        needs_update = true;
}

void gc_cardman_set_idx(uint16_t idx_num) {
    if (idx_num != card_idx)
        needs_update = true;
    if ((idx_num >= IDX_MIN) && (idx_num < UINT16_MAX)) {
        card_idx = idx_num;
        card_chan = CHAN_MIN;
    }
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
}

void gc_cardman_next_idx(void) {
    switch (cardman_state) {
        case GC_CM_STATE_NAMED:
            if (!try_set_prev_named_card()
                && !try_set_boot_card()
                && !try_set_game_id_card())
                set_default_card();
            break;
        case GC_CM_STATE_BOOT:
            if (!try_set_game_id_card())
                set_default_card();
            break;
        case GC_CM_STATE_GAMEID: set_default_card(); break;
        case GC_CM_STATE_NORMAL:
            card_idx += 1;
            card_chan = CHAN_MIN;
            if (card_idx > UINT16_MAX)
                card_idx = UINT16_MAX;
            snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
            break;
    }

    needs_update = true;
}

void gc_cardman_prev_idx(void) {
    switch (cardman_state) {
        case GC_CM_STATE_NAMED:
        case GC_CM_STATE_BOOT:
            if (!try_set_next_named_card())
                set_default_card();
            break;
        case GC_CM_STATE_GAMEID:
            if (!try_set_boot_card())
                if (!try_set_next_named_card())
                    set_default_card();
            break;
        case GC_CM_STATE_NORMAL:
            card_idx -= 1;
            card_chan = CHAN_MIN;
            if (card_idx <= GC_CARD_IDX_SPECIAL) {
                if (!try_set_game_id_card() && !try_set_boot_card() && !try_set_next_named_card())
                    set_default_card();
            } else {
                snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
            }
            break;
    }

    needs_update = true;
}

int gc_cardman_get_idx(void) {
    return card_idx;
}

int gc_cardman_get_channel(void) {
    return card_chan;
}

void gc_cardman_set_gameid(const char *const card_game_id) {
    if (!settings_get_gc_game_id())
        return;

    char new_folder_name[MAX_FOLDER_NAME_LENGTH] = {};
    if (card_game_id[0]) {
        card_config_get_card_folder(card_game_id, new_folder_name, sizeof(new_folder_name));
        if (new_folder_name[0] == 0x00)
            snprintf(new_folder_name, sizeof(new_folder_name), "%s", card_game_id);
        log(LOG_TRACE, "Folder: %s\n", new_folder_name);
        if ((strcmp(new_folder_name, folder_name) != 0) || (GC_CM_STATE_GAMEID != cardman_state)) {
            card_idx = GC_CARD_IDX_SPECIAL;
            cardman_state = GC_CM_STATE_GAMEID;
            card_chan = CHAN_MIN;
            memcpy(folder_name, new_folder_name, sizeof(folder_name));
            needs_update = true;
        }
    }
}

void gc_cardman_set_progress_cb(cardman_cb_t func) {
    cardman_cb = func;
}

char *gc_cardman_get_progress_text(void) {
    static char progress[32];

    if (cardman_operation != CARDMAN_IDLE)
        snprintf(progress, sizeof(progress), "%s %.2f kB/s", cardman_operation == CARDMAN_CREATE ? "Wr" : "Rd",
                 1000000.0 * cardprog_pos / (time_us_64() - cardprog_start) / 1024);
    else
        snprintf(progress, sizeof(progress), "Switching...");

    return progress;
}

uint32_t gc_cardman_get_card_size(void) {
    return card_size;
}

const char *gc_cardman_get_folder_name(void) {
    return folder_name;
}

gc_cardman_state_t gc_cardman_get_state(void) {
    return cardman_state;
}

void gc_cardman_force_update(void) {
    needs_update = true;
}


bool gc_cardman_needs_update(void) {
    return needs_update;
}

bool __time_critical_func(gc_cardman_is_accessible)(void) {
    return true;
}

bool gc_cardman_is_idle(void) {
    return cardman_operation == CARDMAN_IDLE;
}

void gc_cardman_init(void) {
    cardman_operation = CARDMAN_IDLE;
    cardman_state = GC_CM_STATE_NORMAL;
    set_default_card();

}

void gc_cardman_task(void) {
    gc_cardman_continue();
}
