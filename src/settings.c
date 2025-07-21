#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "pico/multicore.h"
#include "sd.h"
#include "wear_leveling/wear_leveling.h"

#include "ini.h"

/* NOTE: for any change to the layout/size of this structure (that gets shipped to users),
   ensure to increase the version magic below -- this will trigger setting reset on next boot */
typedef struct {
    uint32_t version_magic;
    uint16_t gc_card;
    uint8_t pad_0[2];
    uint8_t gc_channel;
    uint8_t gc_boot_channel;
    uint8_t gc_flags; // TODO: single bit options
    uint8_t sys_flags; // TODO: single bit options: whether gc or gc mode, etc
    uint8_t display_timeout; // display - auto off, in seconds, 0 - off
    uint8_t display_contrast; // display - contrast, 0-255
    uint8_t display_vcomh; // display - vcomh, valid values are 0x00, 0x20, 0x30 and 0x40
    uint8_t gc_cardsize;
} settings_t;

typedef struct {
    uint8_t gc_flags;
    uint8_t sys_flags;
    uint8_t gc_cardsize;
} serialized_settings_t;

#define SETTINGS_UPDATE_FIELD(field) settings_update_part(&settings.field, sizeof(settings.field))

#define SETTINGS_VERSION_MAGIC             (0xAACE0000)
#define SETTINGS_GC_FLAGS_AUTOBOOT         (0b0000001)
#define SETTINGS_GC_FLAGS_GAME_ID          (0b0000010)
#define SETTINGS_GC_FLAGS_ENC              (0b0000100)  // Card Encoding Default is Japanese
#define SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY (0b0000010)
#define SETTINGS_SYS_FLAGS_SHOW_INFO       (0b0000100)

_Static_assert(sizeof(settings_t) == 16, "unexpected padding in the settings structure");

static settings_t settings;
static serialized_settings_t serialized_settings;
static const char settings_path[] = "/.flippermce/settings.ini";

static void settings_update_part(void *settings_ptr, uint32_t sz);
static void settings_serialize(void);

static int parse_card_configuration(void *user, const char *section, const char *name, const char *value) {
    serialized_settings_t* _s = user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    #define DIFFERS(v, s) ((strcmp(v, "ON") == 0) != s)
    if (MATCH("GC", "Autoboot")
        && DIFFERS(value, ((_s->gc_flags & SETTINGS_GC_FLAGS_AUTOBOOT) > 0))) {
        _s->gc_flags ^= SETTINGS_GC_FLAGS_AUTOBOOT;
    } else if (MATCH("GC", "GameID")
        && DIFFERS(value, ((_s->gc_flags & SETTINGS_GC_FLAGS_GAME_ID) > 0))) {
        _s->gc_flags ^= SETTINGS_GC_FLAGS_GAME_ID;
    } else if (MATCH("GC", "Encoding")
        && (strcmp(value, "JAP") != 0) != ((_s->gc_flags & SETTINGS_GC_FLAGS_ENC) > 0)) {
        _s->gc_flags ^= SETTINGS_GC_FLAGS_ENC;
    } else if (MATCH("GC", "CardSize")) {
        int size = atoi(value);
        switch (size) {
            case 4:
            case 8:
            case 16:
            case 32:
            case 64:
                _s->gc_cardsize = size;
                break;
            default:
                break;
        }
    } else if (MATCH("General", "FlippedScreen")
        && DIFFERS(value, ((_s->sys_flags & SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY) > 0))) {
        _s->sys_flags ^= SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY;
    } else if (MATCH("General", "ShowInfo")
        && DIFFERS(value, ((_s->sys_flags & SETTINGS_SYS_FLAGS_SHOW_INFO) > 0))) {
        _s->sys_flags ^= SETTINGS_SYS_FLAGS_SHOW_INFO;
    }
    #undef MATCH
    return 1;
}

static void settings_deserialize(void) {
    int fd;

    fd = sd_open(settings_path, O_RDONLY);
    if (fd >= 0) {

        serialized_settings_t newSettings = {.gc_flags = settings.gc_flags,
                                             .sys_flags = settings.sys_flags,
                                             .gc_cardsize = settings.gc_cardsize};
        serialized_settings = newSettings;
        ini_parse_sd_file(fd, parse_card_configuration, &newSettings);
        sd_close(fd);
        if (memcmp(&newSettings, &serialized_settings, sizeof(serialized_settings))) {
            printf("Updating settings from ini\n");
            serialized_settings = newSettings;
            settings.sys_flags       = newSettings.sys_flags;
            settings.gc_flags       = newSettings.gc_flags;
            settings.gc_cardsize    = newSettings.gc_cardsize;

            wear_leveling_write(0, &settings, sizeof(settings));
        }
    }
}

static void settings_serialize(void) {
    int fd;
    // Only serialize if required
    if (serialized_settings.gc_cardsize == settings.gc_cardsize &&
        serialized_settings.gc_flags == settings.gc_flags &&
        serialized_settings.sys_flags == settings.sys_flags) {
        return;
    }

    if (!sd_exists("/.flippermce/")) {
        sd_mkdir("/.flippermce/");
    }
    fd = sd_open(settings_path, O_RDWR | O_CREAT);
    if (fd >= 0) {
        printf("Serializing Settings\n");
        char line_buffer[256] = { 0x0 };
        int written = snprintf(line_buffer, 256, "[General]\n");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "FlippedScreen=%s\n", ((settings.sys_flags & SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "ShowInfo=%s\n", ((settings.sys_flags & SETTINGS_SYS_FLAGS_SHOW_INFO) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "[GC]\n");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "Autoboot=%s\n", ((settings.gc_flags & SETTINGS_GC_FLAGS_AUTOBOOT) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "GameID=%s\n", ((settings.gc_flags & SETTINGS_GC_FLAGS_GAME_ID) > 0) ? "ON" : "OFF");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "Encoding=%s\n", ((settings.gc_flags & SETTINGS_GC_FLAGS_GAME_ID) > 0) ? "JAP" : "WORLD");
        sd_write(fd, line_buffer, written);
        written = snprintf(line_buffer, 256, "CardSize=%u\n", settings.gc_cardsize);
        sd_write(fd, line_buffer, written);

        sd_close(fd);
    }
    serialized_settings.sys_flags       = settings.sys_flags;
    serialized_settings.gc_flags       = settings.gc_flags;
    serialized_settings.gc_cardsize    = settings.gc_cardsize;
}

static void settings_reset(void) {
    memset(&settings, 0, sizeof(settings));
    settings.version_magic = SETTINGS_VERSION_MAGIC;
    settings.display_timeout = 0; // off
    settings.display_contrast = 255; // 100%
    settings.display_vcomh = 0x30; // 0.83 x VCC
    settings.gc_flags = SETTINGS_GC_FLAGS_GAME_ID | SETTINGS_GC_FLAGS_AUTOBOOT;
    settings.gc_cardsize = 64;
    if (wear_leveling_write(0, &settings, sizeof(settings)) == WEAR_LEVELING_FAILED)
        fatal("failed to reset settings");
}

void settings_load_sd(void) {
    sd_init();
    if (sd_exists(settings_path)) {
        printf("Reading settings from %s\n", settings_path);
        settings_deserialize();
    } else {
        settings_serialize();
    }
}

void settings_init(void) {
    printf("Settings - init\n");
    if (wear_leveling_init() == WEAR_LEVELING_FAILED) {
        printf("failed to init wear leveling, reset settings\n");
        settings_reset();

        if (wear_leveling_init() == WEAR_LEVELING_FAILED)
            fatal("cannot init eeprom emu");
    }

    wear_leveling_read(0, &settings, sizeof(settings));

    if (settings.version_magic != SETTINGS_VERSION_MAGIC) {
        printf("version magic mismatch, reset settings\n");
        settings_reset();
    }

}

static void settings_update_part(void *settings_ptr, uint32_t sz) {
    if (multicore_lockout_victim_is_initialized(1))
       multicore_lockout_start_blocking();
    wear_leveling_write((uint8_t*)settings_ptr - (uint8_t*)&settings, settings_ptr, sz);
    if (multicore_lockout_victim_is_initialized(1))
        multicore_lockout_end_blocking();
    settings_serialize();
}


int settings_get_gc_card(void) {
    if (settings.gc_card < IDX_MIN)
        return IDX_MIN;
    return settings.gc_card;
}

int settings_get_gc_channel(void) {
    if (settings.gc_channel < CHAN_MIN)
        return CHAN_MIN;
    return settings.gc_channel;
}

int settings_get_gc_boot_channel(void) {
    if (settings.gc_boot_channel < CHAN_MIN)
        return CHAN_MIN;
    return settings.gc_boot_channel;
}

uint8_t settings_get_gc_cardsize(void) {
    return settings.gc_cardsize;
}

void settings_set_gc_card(int card) {
    if (card != settings.gc_card) {
        settings.gc_card = card;
        SETTINGS_UPDATE_FIELD(gc_card);
    }
}

void settings_set_gc_channel(int chan) {
    if (chan != settings.gc_channel) {
        settings.gc_channel = chan;
        SETTINGS_UPDATE_FIELD(gc_channel);
    }
}

void settings_set_gc_boot_channel(int chan) {
    if (chan != settings.gc_boot_channel) {
        settings.gc_boot_channel = chan;
        SETTINGS_UPDATE_FIELD(gc_boot_channel);
    }
}

void settings_set_gc_cardsize(uint8_t size) {
    if (size != settings.gc_cardsize) {
        settings.gc_cardsize = size;
        SETTINGS_UPDATE_FIELD(gc_cardsize);
    }
}

bool settings_get_gc_autoboot(void) {
    return (settings.gc_flags & SETTINGS_GC_FLAGS_AUTOBOOT);
}

void settings_set_gc_autoboot(bool autoboot) {
    if (autoboot != settings_get_gc_autoboot())
        settings.gc_flags ^= SETTINGS_GC_FLAGS_AUTOBOOT;
    SETTINGS_UPDATE_FIELD(gc_flags);
}

bool settings_get_gc_game_id(void) {
    return (settings.gc_flags & SETTINGS_GC_FLAGS_GAME_ID);
}

void settings_set_gc_game_id(bool enabled) {
    if (enabled != settings_get_gc_game_id())
        settings.gc_flags ^= SETTINGS_GC_FLAGS_GAME_ID;
    SETTINGS_UPDATE_FIELD(gc_flags);
}

bool settings_get_gc_encoding(void) {
    return (settings.gc_flags & SETTINGS_GC_FLAGS_ENC);
}

void settings_set_gc_encoding(bool enabled) {
    if (enabled != settings_get_gc_encoding())
        settings.gc_flags ^= SETTINGS_GC_FLAGS_ENC;
    SETTINGS_UPDATE_FIELD(gc_flags);
}

uint8_t settings_get_display_timeout() {
    return settings.display_timeout;
}

uint8_t settings_get_display_contrast() {
    return settings.display_contrast;
}

uint8_t settings_get_display_vcomh() {
    return settings.display_vcomh;
}

bool settings_get_display_flipped() {
    return (settings.sys_flags & SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY);
}

bool settings_get_show_info() {
    return (settings.sys_flags & SETTINGS_SYS_FLAGS_SHOW_INFO);
}

void settings_set_display_timeout(uint8_t display_timeout) {
    settings.display_timeout = display_timeout;
    SETTINGS_UPDATE_FIELD(display_timeout);
}

void settings_set_display_contrast(uint8_t display_contrast) {
    settings.display_contrast = display_contrast;
    SETTINGS_UPDATE_FIELD(display_contrast);
}

void settings_set_display_vcomh(uint8_t display_vcomh) {
    settings.display_vcomh = display_vcomh;
    SETTINGS_UPDATE_FIELD(display_vcomh);
}

void settings_set_display_flipped(bool flipped) {
    if (flipped != settings_get_display_flipped())
        settings.sys_flags ^= SETTINGS_SYS_FLAGS_FLIPPED_DISPLAY;
    SETTINGS_UPDATE_FIELD(sys_flags);
}

void settings_set_show_info(bool show) {
    if (show != settings_get_show_info())
        settings.sys_flags ^= SETTINGS_SYS_FLAGS_SHOW_INFO;
    SETTINGS_UPDATE_FIELD(sys_flags);
}
