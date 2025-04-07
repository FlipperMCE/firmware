#ifndef GC_UNLOCK_H
#define GC_UNLOCK_H

#include <stdint.h>

// Function declarations
static void gc_unlock_transform(uint32_t data, uint32_t lastdata, uint32_t *cntxt, uint8_t rotate);
static uint32_t gc_unlock_hash(const uint8_t *data, uint16_t length);



#endif /* GC_UNLOCK_H */