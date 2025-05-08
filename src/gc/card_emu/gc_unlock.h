#ifndef GC_UNLOCK_H
#define GC_UNLOCK_H

#include <stdint.h>

// Function declarations
void mc_unlock(void);
void mc_unlock_stage_0(uint32_t offset_u32);

#endif /* GC_UNLOCK_H */