#include <inttypes.h>
#include <stdint.h>

#define CACHE_SIZE  512 * 45

typedef union {
    struct {
        uint16_t dirty_heap[8 * 1024 * 1024 / 512];
        uint8_t dirty_map[8 * 1024 * 1024 / 512 / 8];
    } gc;

} bigmem_t;


extern bigmem_t bigmem;
extern uint8_t cache[CACHE_SIZE];
