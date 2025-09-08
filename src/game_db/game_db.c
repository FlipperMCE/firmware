
#include "game_db.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/platform.h"

#include "debug.h"
#include "sd.h"
#include "settings.h"

#define MAX_GAME_NAME_LENGTH (127)
#define MAX_PREFIX_LENGTH    (5)
#define MAX_STRING_ID_LENGTH (10)
#define MAX_PATH_LENGTH      (64)

extern const char _binary_gamedbgc_dat_start, _binary_gamedbgc_dat_size;

typedef struct {
    size_t offset;
    uint32_t game_id;
    const char* region;
    const char* game_id_char;
    const char* name;
} game_lookup;

static game_lookup current_game;

#pragma GCC diagnostic ignored "-Warray-bounds"
static uint32_t game_db_char_array_to_uint32(const char in[4]) {
    char inter[4] = {in[3], in[2], in[1], in[0  ]};
    uint32_t tgt;
    memcpy((void*)&tgt, (void*)inter, sizeof(tgt));
    return tgt;
}
#pragma GCC diagnostic pop

static game_lookup build_game_lookup(const char* const db_start, const size_t db_size, const size_t offset) {
    game_lookup game = {};
    size_t name_offset;
    game.game_id = game_db_char_array_to_uint32(&(db_start)[offset]);
    game.offset = offset;
    name_offset = game_db_char_array_to_uint32(&(db_start)[offset + 4]);
    game.game_id_char = &(db_start)[offset];
    game.region = &(db_start)[offset + 8];

    if ((name_offset < db_size) && ((db_start)[name_offset] != 0x00))
        game.name = &((db_start)[name_offset]);
    else
        game.name = NULL;

    return game;
}

static game_lookup find_game_lookup(const char* game_id) {
    uint32_t numeric_id = 0;

    const char* const db_start = &_binary_gamedbgc_dat_start;
    const char* const db_size = &_binary_gamedbgc_dat_size;

    game_lookup ret = {
        .game_id = 0U,
        .region = NULL,
        .game_id_char = NULL,
        .name = NULL,
    };


    if (game_id != NULL && game_id[0]) {
        numeric_id = game_db_char_array_to_uint32(game_id);
    }
    if (numeric_id != 0) {

        uint32_t offset = 0;
        game_lookup game;
        do {
            game = build_game_lookup(db_start, (size_t)db_size, offset);

            if (game.game_id == numeric_id) {
                ret = game;
            }
            offset += 12;
        } while ((game.game_id != 0) && (offset < (size_t)db_size) && (ret.game_id == 0));

    }

    return ret;
}

void game_db_get_current_name(char* const game_name) {
    strlcpy(game_name, "", MAX_GAME_NAME_LENGTH);

    if ((current_game.name != NULL) && (current_game.name[0] != 0)) {
        strlcpy(game_name, current_game.name, MAX_GAME_NAME_LENGTH);
    }
}

void game_db_get_current_id(const char** const id, const char** region) {

    if (current_game.game_id != 0) {
        *id = current_game.game_id_char;
        *region = current_game.region;
    } else {
        *id = NULL;
        *region = NULL;
    }

}

void game_db_get_current_region(const char** region) {
    if (current_game.region != NULL) {
        *region = current_game.region;
    } else {
        *region = NULL;
    }
}

void game_db_update_game(const char* const game_id) {
    current_game = find_game_lookup(game_id);
}

void game_db_extract_game_id(const char* const game_id, char* const game_id_out) {
    if ((strlen(game_id) == MAX_GAME_ID_LENGTH-1) && (memcmp(game_id, "DL-DOL", 6) == 0)) {
        memcpy(game_id_out, game_id + 7, 4);
    } else {
        memset(game_id_out, 0x00, 4);
    }
}

void game_db_get_game_name(const char* game_id, char* game_name) {
    char game_id_out[5] = {0x00};
    if (!game_id || game_id[0] == 0)
        return;

    game_db_extract_game_id(game_id, game_id_out);
    game_lookup lookup = find_game_lookup(game_id_out);
    if (lookup.name && lookup.name[0])
        strlcpy(game_name, lookup.name, MAX_GAME_NAME_LENGTH);

}

void game_db_init(void) {
    current_game.game_id = 0U;
    current_game.name = NULL;
    current_game.region = NULL;
    current_game.game_id_char = NULL;
}