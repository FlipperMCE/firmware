#pragma once

#include <inttypes.h>
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/platform.h"

#include "util.h"

extern spin_lock_t *gc_dirty_spin_lock;
extern volatile uint32_t gc_dirty_lockout;

static inline void __time_critical_func(gc_dirty_lock)(void) {
    spin_lock_unsafe_blocking(gc_dirty_spin_lock);
}

static inline void __time_critical_func(gc_dirty_unlock)(void) {
    spin_unlock_unsafe(gc_dirty_spin_lock);
}

static inline void __time_critical_func(gc_dirty_lockout_renew)(void) {
    /* lockout for 100ms, store time in ms */
    gc_dirty_lockout = (uint32_t)(RAM_time_us_64() / 1000) + 100;
}

static inline int __time_critical_func(gc_dirty_lockout_expired)(void) {
    return gc_dirty_lockout < (uint32_t)(RAM_time_us_64() / 1000);
}

void gc_dirty_init(void);
int gc_dirty_get_marked(void);
void gc_dirty_mark(uint32_t sector);
void gc_dirty_task(void);

extern int gc_dirty_activity;
