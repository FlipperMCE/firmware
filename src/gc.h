#pragma once
#include <stdbool.h>

void gc_switch_card(void);
void gc_init(void);
bool gc_task(void);
void gc_deinit(void);
