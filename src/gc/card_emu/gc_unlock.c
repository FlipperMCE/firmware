// Includes
#include "gc_unlock.h"
#include <debug.h>
#include <stdint.h>
#include "card_emu/gc_mc_data_interface.h"
#include "gc_memory_card.h"
#include "gc_mc_internal.h"
#include "hardware/dma.h"
#include "pico/platform.h"


// Transform and hash functions from memory card decryption

#define _ROTL(v,s) (s&31 ? ((uint32_t)v<<s)|((uint32_t)v>>(0x20-s)) : v)

int unlock_stage = 0;
static uint32_t card_cipher;


static void extract_flash_id(uint8_t *flash_id,  uint8_t *serial, uint64_t time)
{
    // Create flash_id from base_serial
    uint64_t rand = time;

    for (int i = 0; i < 12; i++)
    {
      rand = (((rand * (uint64_t)0x0000000041c64e6dULL) + (uint64_t)0x0000000000003039ULL) >> 16);
      flash_id[i] = serial[i] - ((uint8_t)rand & 0xff);
      rand = (((rand * (uint64_t)0x0000000041c64e6dULL) + (uint64_t)0x0000000000003039ULL) >> 16);
      rand &= (uint64_t)0x0000000000007fffULL;
    }
}

static uint32_t exnor_1st(uint32_t a,uint32_t b)
{
	uint32_t c,d,e,f,r1,r2,r3,r4;

	c = 0;
	while(c<b) {
		d = (a>>23);
		e = (a>>15);
		f = (a>>7);
		r1 = (a^f);
		r2 = (e^r1);
		r3 = ~(d^r2);		//eqv(d,r2)
		e = (a>>1);
		r4 = ((r3<<30)&0x40000000);
		a = (e|r4);
		c++;
	};
	return a;
}


static uint32_t exnor(uint32_t a,uint32_t b)
{
	uint32_t c,d,e,f,r1,r2,r3,r4;

	c = 0;
	while(c<b) {
		d = (a<<23);
		e = (a<<15);
		f = (a<<7);
		r1 = (a^f);
		r2 = (e^r1);
		r3 = ~(d^r2);		//eqv(d,r2)
		e = (a<<1);
		r4 = ((r3>>30)&0x02);
		a = (e|r4);
		c++;
	};
	return a;
}



static uint32_t bitrev(uint32_t val)
{
	uint32_t cnt,val1,ret,shift,shift1;

	cnt = 0;
	ret = 0;
	shift = 1;
	shift1 = 0;
	while(cnt<32) {
		if(cnt<=15) {
			val1 = val&(1<<cnt);
			val1 <<= ((31-cnt)-shift1);
			ret |= val1;
			shift1++;
		} else if(cnt==31) {
			val1 = val>>31;
			ret |= val1;
		} else {
			val1 = 1;
			val1 = val&(1<<cnt);
			val1 >>= shift;
			ret |= val1;
			shift += 2;
		}
		cnt++;
	}
	return ret;
}

static void __time_critical_func(init_cipher)(uint32_t* cipher, uint32_t offset, uint32_t count) {
    uint32_t val = exnor_1st(offset,(count<<3)+1);
    {
        uint32_t a,b,c,r1,r2,r3;
        a = (val>>23);
        b = (val>>15);
        c = (val>>7);
        r1 = (val^c);
        r2 = (b^r1);
        r3 = ~(a^r2);		//eqv(a,r2)
        r1 = (val|(r3<<31));
        *cipher = bitrev(r1);
    }
}

static void __time_critical_func(update_cipher)(uint32_t* cipher, uint32_t count) {
    uint32_t val = exnor(*cipher,count);
    {
        uint32_t a,b,c,r1,r2,r3;
        a = (val<<23);
        b = (val<<15);
        c = (val<<7);
        r1 = (val^c);
        r2 = (b^r1);
        r3 = ~(a^r2);		//eqv(a,r2)
        r1 = (val|(r3>>31));
        *cipher = r1;
    }
}

// readArrayUnlock len is 4..32
// readArrayUnlock address is 0x7FEC8000..0x7FECF000

void __time_critical_func(mc_unlock)(void) {
    uint8_t offset[4] = { 0x00, 0x00, 0x00, 0x00 };
    uint32_t offset_u32 = 0U;
    uint32_t len = 0;
    uint8_t _;

    if (unlock_stage == 0) {
        gc_receiveOrNextCmd(&offset[3]);
        gc_receiveOrNextCmd(&offset[2]);
        gc_receiveOrNextCmd(&offset[1]);
        gc_receiveOrNextCmd(&offset[0]);
        offset_u32 = ((offset[0] << 29) & 0x60000000) | ((offset[1] << 21) & 0x1FE00000) | ((offset[2] << 19) & 0x00180000) | ((offset[3] << 12) & 0x0007F000);
        while (gc_receive(&_) != RECEIVE_RESET) {
            len++;
        }
        len -= 256;
        DPRINTF("Unlock: %08x / %u\n", offset_u32, len);
        init_cipher(&card_cipher, offset_u32, len+1);

        DPRINTF("Cipher is %08x\n", card_cipher);
        unlock_stage++;
    } else if (unlock_stage == 1) {
        uint32_t a,b,c, d, e;
        uint8_t *a_pu8 = (uint8_t*)&a,
                *b_pu8 = (uint8_t*)&b,
                *c_pu8 = (uint8_t*)&c,
                *d_pu8 = (uint8_t*)&d,
                *e_pu8 = (uint8_t*)&e;
        gc_receiveOrNextCmd(&offset[1]);
        gc_receiveOrNextCmd(&offset[0]);
        offset_u32 = ((offset[0] << 24) & 0xFF000000) | ((offset[1] << 16) & 0x00FF0000);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);

        dma_channel_start(DMA_WAIT_CHAN);
        //len -= 20;
        DPRINTF("Unlock: %08x / %u\n", offset_u32, len);
        gc_mc_data_interface_setup_read_page(0, true, false);

        volatile gc_mcdi_page_t *page = gc_mc_data_interface_get_page(0);
        uint8_t flash_id[12] = {0x00};

        extract_flash_id(flash_id,page->data, swap64(*(uint64_t*)&page->data[12]));

        a = swap32(((uint32_t*)flash_id)[0]) ^ card_cipher;
        update_cipher(&card_cipher, 32);
        b = swap32(((uint32_t*)flash_id)[1]) ^ card_cipher;
        update_cipher(&card_cipher, 32);
        c = swap32(((uint32_t*)flash_id)[2]) ^ card_cipher;
        update_cipher(&card_cipher, 32);
        d = swap32(((uint32_t*)&page->data)[12]) ^ card_cipher;
        update_cipher(&card_cipher, 32);
        e = swap32(((uint32_t*)&page->data)[16]) ^ card_cipher;

        while (dma_channel_is_busy(DMA_WAIT_CHAN)); // Wait for DMA to complete

        gc_mc_respond(a_pu8[3]);
        gc_mc_respond(a_pu8[2]);
        gc_mc_respond(a_pu8[1]);
        gc_mc_respond(a_pu8[0]);
        gc_mc_respond(b_pu8[3]);
        gc_mc_respond(b_pu8[2]);
        gc_mc_respond(b_pu8[1]);
        gc_mc_respond(b_pu8[0]);
        gc_mc_respond(c_pu8[3]);
        gc_mc_respond(c_pu8[2]);
        gc_mc_respond(c_pu8[1]);
        gc_mc_respond(c_pu8[0]);
        gc_mc_respond(d_pu8[3]);
        gc_mc_respond(d_pu8[2]);
        gc_mc_respond(d_pu8[1]);
        gc_mc_respond(d_pu8[0]);
        gc_mc_respond(e_pu8[3]);
        gc_mc_respond(e_pu8[2]);
        gc_mc_respond(e_pu8[1]);
        gc_mc_respond(e_pu8[0]);

        DPRINTF("Key is %08x %08x %08x \n", a, b, c);
        unlock_stage++;
    } else {
        if (unlock_stage++ > 2) {
            card_state = 0x41;
        }
    }
    return;

}