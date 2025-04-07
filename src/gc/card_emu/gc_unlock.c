// Includes
#include "gc_unlock.h"
#include "gc_memory_card.h"


// Transform and hash functions from memory card decryption

#define _ROTL(v,s) (s&31 ? ((uint32_t)v<<s)|((uint32_t)v>>(0x20-s)) : v)

static void gc_unlock_transform(uint32_t data, uint32_t lastdata, uint32_t *cntxt, uint8_t rotate)
{
    uint32_t input;

    input = ((data<<8)&0xF000)|((lastdata<<4)&0x0F00)|((data<<4)&0x00F0)|(lastdata&0x000F);
    if (input & 0x0080)
        input ^= 0xFF00;

    cntxt[0] += input;
    cntxt[1] += _ROTL(((cntxt[2]^cntxt[3])+cntxt[0]), (32-(rotate&31)));
    cntxt[2] = (cntxt[3]&cntxt[0]) | ((cntxt[0]^0xFFFF0000)&cntxt[1]);
    cntxt[3] = cntxt[0] ^ cntxt[1] ^ cntxt[2];
}

static uint32_t gc_unlock_hash(const uint8_t *data, uint16_t length)
{
    uint32_t cntxt[4];
    uint8_t rotate;
    uint16_t i;

    cntxt[0] = 0;
    cntxt[1] = 0x05EFE0AA;
    cntxt[2] = 0xDAF4B157;
    cntxt[3] = 0x6BBEC3B6;

    for (i=0; i < length; i++)
        cntxt[0] += data[i];

    rotate = cntxt[0]+9;
    cntxt[0] += 0x170A7489;

    while (--length)
    {
        gc_unlock_transform(data[1], data[0], cntxt, rotate++);
        data++;
    }

    return cntxt[1];
}

