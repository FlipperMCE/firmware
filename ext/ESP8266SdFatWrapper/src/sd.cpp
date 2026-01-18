#include "config.h"

#include "SdFat.h"
#include "SPI.h"

#include "hardware/gpio.h"
#include "pico/platform.h"

extern "C" {
#include "debug.h"
#include "sd.h"
}

#include <stdio.h>

#ifndef NUM_FILES
    #define NUM_FILES 16
#endif

static SdFat sd;
static File files[NUM_FILES + 1];
static bool initialized = false;

extern "C" void sd_init(bool reinit) {
    if (reinit) {
        sd.volumeBegin();
        return;
    }
    if (!initialized) {
        SD_PERIPH.setRX(SD_MISO);
        SD_PERIPH.setTX(SD_MOSI);
        SD_PERIPH.setSCK(SD_SCK);
        SD_PERIPH.setCS(SD_CS);
        gpio_set_drive_strength(SD_SCK, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_drive_strength(SD_MOSI, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_drive_strength(SD_CS, GPIO_DRIVE_STRENGTH_12MA);


        int ret = sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_BAUD, &SD_PERIPH));

        cid_t cid;
        if (sd.card()->readCID(&cid)) {
            DPRINTF("SD Card CID:\n");
            DPRINTF(" Manufacturer ID: 0x%02X\n", cid.mid);
            DPRINTF(" OEM ID: %.2s\n", cid.oid);
            DPRINTF(" Product: %.5s\n", cid.pnm);
            DPRINTF(" Revision: %d.%d\n", cid.prv_n, cid.prv_m);
            DPRINTF(" Serial number: 0x%08X\n", cid.psn);
            DPRINTF(" Manufacturing date: %02d/%04d\n",
                cid.mdt_month, 2000 + ((cid.mdt_year_high << 4) | cid.mdt_year_low));
        } else {
            DPRINTF("failed to read CID\n");
        }

        if (ret != 1) {
            if (sd.sdErrorCode()) {
                fatal("failed to mount the card\nSdError: 0x%02X,0x%02X\ncheck the card", sd.sdErrorCode(), sd.sdErrorData());
            } else if (!sd.fatType()) {
                fatal("failed to mount the card\ncheck the card is formatted correctly");
            } else {
                fatal("failed to mount the card\nUNKNOWN");
            }
        }
        initialized = true;
    }
}

extern "C" void sd_unmount() {
    if (initialized) {
        sd_sync_cache();
        sd.end();
    }
}

void sdCsInit(SdCsPin_t pin) {
    gpio_init(pin);
    gpio_set_dir(pin, 1);
}

void sdCsWrite(SdCsPin_t pin, bool level) {
    gpio_put(pin, level);
}

extern "C" int sd_open(const char *path, int oflag) {
    size_t fd;
    if (!initialized) {
        sd_init(false);
    }

    if (!sd_exists(path) && (oflag & O_CREAT) == 0) {
        return -1;
    }

    for (fd = 0; fd < NUM_FILES; ++fd)
        if (!files[fd].isOpen())
            break;

    /* no fd available */
    if (fd >= NUM_FILES)
        return -1;

    files[fd].open(path, oflag);

    /* error during opening file */
    if (!files[fd].isOpen())
        return -1;

    return fd;
}

#define CHECK_FD(fd) if (fd >= NUM_FILES || !files[fd].isOpen()) return -1;
#define CHECK_FD_VOID(fd) if (fd >= NUM_FILES || !files[fd].isOpen()) return;

extern "C" int sd_close(int fd) {
    CHECK_FD(fd);

    return files[fd].close() != true;
}

extern "C" void sd_flush(int fd) {
    CHECK_FD_VOID(fd);

    files[fd].flush();
}

extern "C" int sd_read(int fd, void *buf, size_t count) {
    CHECK_FD(fd);

    return files[fd].read(buf, count);
}

extern "C" int sd_write(int fd, void *buf, size_t count) {
    CHECK_FD(fd);

    return files[fd].write(buf, count);
}

extern "C" int sd_seek(int fd, int32_t offset, int whence) {
    CHECK_FD(fd);

    if (whence == 0) {
        return files[fd].seekSet(offset) != true;
    } else if (whence == 1) {
        return files[fd].seekCur(offset) != true;
    } else if (whence == 2) {
        return files[fd].seekEnd(offset) != true;
    }

    return 1;
}

extern "C" uint32_t sd_tell(int fd) {
    CHECK_FD(fd);

    return (uint32_t)files[fd].curPosition();
}

extern "C" int sd_mkdir(const char *path) {
    if (sd_exists(path)) {
        /* return 0 if the directory already exists */
        return 0;
    } else {
        /* return 1 on error */
        return sd.mkdir(path) != true;
    }
}

extern "C" int sd_exists(const char *path) {
    return sd.exists(path);
}

extern "C" int sd_filesize(int fd) {
    CHECK_FD(fd);
    return files[fd].fileSize();
}

extern "C" int sd_rmdir(const char* path) {
    /* return 1 on error */
    return sd.rmdir(path) != true;
}

extern "C" int sd_remove(const char* path) {
    /* return 1 on error */
    return sd.remove(path) != true;
}

extern "C" int sd_iterate_dir(int dir, int it) {
    if (it == -1) {
        for (it = 0; it < NUM_FILES; ++it)
            if (!files[it].isOpen())
                break;
    }
    if (!files[it].openNext(&files[dir], O_RDONLY)) {
        it = -1;
    }
    return it;
}

extern "C" size_t sd_get_name(int fd, char* name, size_t size) {
    return files[fd].getName(name, size);
}

extern "C" bool sd_is_dir(int fd) {
    return files[fd].isDirectory();
}


extern "C" bool sd_read_sector(uint32_t sector, uint8_t* dst) {
    while (sd.card()->isBusy()) {
        // Wait until the card is ready
        tight_loop_contents();
    }
    return sd.card()->readSector(sector, dst);
}


extern "C" bool sd_write_sector(uint32_t sector, const uint8_t* src) {
    while (sd.card()->isBusy()) {
        // Wait until the card is ready
        tight_loop_contents();
    }
    return sd.card()->writeSector(sector, src);
}

extern "C" bool sd_sync_cache(void) {
    // This function must only be called from Core 0
    if (get_core_num() != 0) {
        return false;
    }

    if (!initialized) {
        return true;  // Nothing to sync
    }

    // Sync all open files first
    for (size_t fd = 0; fd < NUM_FILES; ++fd) {
        if (files[fd].isOpen()) {
            files[fd].sync();  // sync includes cache flush for the file
        }
    }

    // Force card sync to ensure all writes are committed
    return sd.card()->syncDevice();
}