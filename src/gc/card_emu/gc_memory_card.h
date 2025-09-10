#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PIN_MC_CONNECTED PIN_SENSE

void gc_memory_card_main(void);
void gc_memory_card_enter(void);
void gc_memory_card_exit(void);
void gc_memory_card_unload(void);
bool gc_memory_card_running(void);