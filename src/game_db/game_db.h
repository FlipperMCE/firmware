#pragma once

#include <stddef.h>
#include <stdint.h>
#include <pico.h>


#define MAX_GAME_ID_LENGTH   (16)

void game_db_get_current_name(char* const game_name);
void game_db_get_current_id(const char** const id, const char** region);
void game_db_get_current_region(const char** region);
void game_db_update_game(const char* const game_id);
void game_db_get_game_name(const char* game_id, char* game_name);
void game_db_extract_game_id(const char* const game_id, char* const game_id_out);

void game_db_init(void);