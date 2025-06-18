// Includes
#include "gc_unlock.h"
#include <debug.h>
#include <stdint.h>
#include "card_emu/gc_mc_data_interface.h"
#include "gc_memory_card.h"
#include "gc_mc_internal.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "pico/platform.h"

#if LOG_LEVEL_GC_UL == 0
#define log(x...)
#else
#define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_GC_UL, level, fmt, ##x)
#endif

// Transform and hash functions from memory card decryption

#define _ROTL(v,s) (s&31 ? ((uint32_t)v<<s)|((uint32_t)v>>(0x20-s)) : v)

int unlock_stage = 0;
static uint32_t card_cipher;

static uint32_t initial_offset_u32 = 0x0;
static uint32_t initial_length_u32 = 0x0;
static uint32_t msg3_len_u32 = 0x0;


static void __time_critical_func(extract_flash_id)(uint8_t *flash_id,  uint8_t *serial, uint64_t time)
{
    // Create flash_id from base_serial
    uint64_t rand = time;
    uint8_t chks = 0;
    //log(LOG_TRACE, "Time is: %016lx \n", time);
    //log(LOG_TRACE, "Serial is ");
    //for (int i = 0; i < 12; i++)
    //    log(LOG_TRACE, "%02x ", serial[i]);
    //log(LOG_TRACE, "\n");
    for (int i = 0; i < 12; i++)
    {
      rand = (((rand * (uint64_t)0x0000000041c64e6dULL) + (uint64_t)0x0000000000003039ULL) >> 16);
      chks += flash_id[i] = serial[i] - ((uint8_t)rand & 0xff);
      rand = (((rand * (uint64_t)0x0000000041c64e6dULL) + (uint64_t)0x0000000000003039ULL) >> 16);
      rand &= (uint64_t)0x0000000000007fffULL;
    }
    chks = chks ^ 0xff;
}

static uint32_t __time_critical_func(exnor_1st)(uint32_t a,uint32_t b)
{
	uint32_t d,e,f,r1,r2,r3,r4;

    for (uint32_t c = 0; c < b; c++) {
		d = (a>>23);
		e = (a>>15);
		f = (a>>7);
		r1 = (a^f);
		r2 = (e^r1);
		r3 = ~(d^r2);		//eqv(d,r2)
		e = (a>>1);
		r4 = ((r3<<30)&0x40000000);
		a = (e|r4);
	};
	return a;
}


static uint32_t __time_critical_func(exnor)(uint32_t a,uint32_t b)
{
	uint32_t d,e,f,r1,r2,r3,r4;

    for (uint32_t c = 0; c < b; c++) {
		d = (a<<23);
		e = (a<<15);
		f = (a<<7);
		r1 = (a^f);
		r2 = (e^r1);
		r3 = ~(d^r2);		//eqv(d,r2)
		e = (a<<1);
		r4 = ((r3>>30)&0x02);
		a = (e|r4);
	};
	return a;
}



static uint32_t __time_critical_func(bitrev)(uint32_t val)
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

void __time_critical_func(mc_unlock_stage_0)(uint32_t offset_u32) {
    uint8_t _;
    dma_channel_start(DMA_WAIT_CHAN);
    initial_offset_u32 = offset_u32;
    initial_length_u32 = 0U;
    while (dma_channel_is_busy(DMA_WAIT_CHAN)); // Wait for DMA to complete

    while (gc_receive(&_) != RECEIVE_RESET) {
        initial_length_u32++;
    }

    unlock_stage = 1;
}

// readArrayUnlock len is 4..32
// readArrayUnlock address is 0x7FEC8000..0x7FECF000

void __time_critical_func(mc_unlock)(void) {
    uint8_t offset[4] = { 0x00, 0x00, 0x00, 0x00 };
    uint32_t offset_u32 = 0U;
    uint8_t _;

    if (unlock_stage == 0) {
        gc_receiveOrNextCmd(&offset[3]);
        gc_receiveOrNextCmd(&offset[2]);
        gc_receiveOrNextCmd(&offset[1]);
        gc_receiveOrNextCmd(&offset[0]);

        dma_channel_start(DMA_WAIT_CHAN);
        DPRINTF("Unlock Msg1: Raw Offset is %02x %02x %02x %02x / %02x\n", offset[3], offset[2], offset[1], offset[0], initial_length_u32);
        offset_u32 = ((offset[3] << 29) & 0x60000000) | ((offset[2] << 21) & 0x1FE00000) | ((offset[1] << 19) & 0x00180000) | ((offset[0] << 12) & 0x0007F000);
        mc_unlock_stage_0(offset_u32);

    } else if (unlock_stage == 1) {
        uint32_t a,b,c, d, e;
        uint8_t len = 0U;
        uint8_t *a_pu8 = (uint8_t*)&a,
                *b_pu8 = (uint8_t*)&b,
                *c_pu8 = (uint8_t*)&c,
                *d_pu8 = (uint8_t*)&d,
                *e_pu8 = (uint8_t*)&e;
        uint8_t flash_id[12] = {0x00};
        unlock_stage++;
        gc_receiveOrNextCmd(&offset[1]);
        gc_receiveOrNextCmd(&offset[0]);
        offset_u32 = ((offset[1] << 24) & 0xFF000000) | ((offset[0] << 16) & 0x00FF0000);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);

        dma_channel_start(DMA_WAIT_CHAN);
        //len -= 20;
        log(LOG_TRACE, "Unlock Msg2: Decoded Offset is %08x / %02x\n", initial_offset_u32, initial_length_u32);
        init_cipher(&card_cipher, initial_offset_u32, initial_length_u32);

        log(LOG_TRACE, "Unlock Msg2: Initial Cipher is %08x\n", card_cipher);

        gc_mc_data_interface_setup_read_page(0, true, false);

        volatile gc_mcdi_page_t *page = gc_mc_data_interface_get_page(0);
        gc_mc_data_interface_wait_for_byte(20);
        extract_flash_id(flash_id,page->data, swap64(*(uint64_t*)(&page->data[12])));

        a = swap32(((uint32_t*)flash_id)[0]) ^ card_cipher;
        update_cipher(&card_cipher, 32);
        b = swap32(((uint32_t*)flash_id)[1]) ^ card_cipher;
        update_cipher(&card_cipher, 32);
        c = swap32(((uint32_t*)flash_id)[2]) ^ card_cipher;
        log(LOG_TRACE, "Unlock Msg2: Last serial cipher is %08x\n", card_cipher);

        // Use 0x00s as challenge to have a predictable response
        update_cipher(&card_cipher, 32);
        d = 0x00000000 ^ card_cipher;
        update_cipher(&card_cipher, 32);
        e = 0x00000000 ^ card_cipher;

        while (dma_channel_is_busy(DMA_WAIT_CHAN)) {tight_loop_contents();}; // Wait for DMA to complete

        gc_mc_respond(a_pu8[3]);
        gc_mc_respond(a_pu8[2]);
        gc_mc_respond(a_pu8[1]);
        gc_mc_respond(a_pu8[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_mc_respond(b_pu8[3]);
        gc_mc_respond(b_pu8[2]);
        gc_mc_respond(b_pu8[1]);
        gc_mc_respond(b_pu8[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_mc_respond(c_pu8[3]);
        gc_mc_respond(c_pu8[2]);
        gc_mc_respond(c_pu8[1]);
        gc_mc_respond(c_pu8[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_mc_respond(d_pu8[3]);
        gc_mc_respond(d_pu8[2]);
        gc_mc_respond(d_pu8[1]);
        gc_mc_respond(d_pu8[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_mc_respond(e_pu8[3]);
        gc_mc_respond(e_pu8[2]);
        gc_mc_respond(e_pu8[1]);
        gc_mc_respond(e_pu8[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);
        gc_receiveOrNextCmd(&offset[0]);

        while (gc_receive(&_) != RECEIVE_RESET) {
            len++;
        }
        update_cipher(&card_cipher,((len + GC_MC_LATENCY_CYCLES) << 3) + 1);

        log(LOG_TRACE, "Unlock Msg2: Serial is %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x  \n",
            flash_id[0], flash_id[1], flash_id[2], flash_id[3],
            flash_id[4], flash_id[5], flash_id[6], flash_id[7],
            flash_id[8], flash_id[9], flash_id[10], flash_id[11]);
        log(LOG_TRACE, "\n");

        log(LOG_TRACE, "Unlock Msg2: Flash ID is ");
        for (int i = 0; i < 12; i++)
            log(LOG_TRACE, "%02x ", flash_id[i]);
        log(LOG_TRACE, "\n");
        log(LOG_TRACE, "Unlock Msg2: Unlock: %08x / %u\n", offset_u32, len);
        log(LOG_TRACE, "Unlock Msg2: Last cipher is %08x\n", card_cipher);
        log(LOG_TRACE, "Unlock Msg2: Key is %08x %08x %08x %08x %08x - len %u \n", a, b, c, d, e, len);
    } else if (unlock_stage == 2) {
        uint32_t a,b,c, d, e;
        uint8_t len = 0;
        uint8_t *a_pu8 = (uint8_t*)&a,
        *b_pu8 = (uint8_t*)&b,
        *c_pu8 = (uint8_t*)&c,
        *d_pu8 = (uint8_t*)&d,
        *e_pu8 = (uint8_t*)&e;
        gc_receiveOrNextCmd(&offset[3]);
        gc_receiveOrNextCmd(&offset[2]);
        offset_u32 = ((offset[3] << 24) & 0xFF000000) | ((offset[2] << 16) & 0x00FF0000);
        gc_receiveOrNextCmd(&offset[1]);
        gc_receiveOrNextCmd(&offset[0]);
        dma_channel_start(DMA_WAIT_CHAN);
        //len -= 20;
        while (dma_channel_is_busy(DMA_WAIT_CHAN)); // Wait for DMA to complete
        gc_mc_respond(0xFF);
        while(gc_receive(&_) != RECEIVE_RESET) {
            len++;
            gc_mc_respond(0xFF);
        }
        msg3_len_u32 = len;
        log(LOG_TRACE, "Unlock Msg3: Raw: %08x / %u\n", *(uint32_t*)offset, len);

        log(LOG_TRACE, "Unlock Msg3: Unlock: %08x / %u\n", offset_u32, len);
        unlock_stage++;

    } else {
        uint32_t a,b,c, d, e;
        uint8_t len = 0;
        uint8_t *a_pu8 = (uint8_t*)&a,
        *b_pu8 = (uint8_t*)&b,
        *c_pu8 = (uint8_t*)&c,
        *d_pu8 = (uint8_t*)&d,
        *e_pu8 = (uint8_t*)&e;
        gc_receiveOrNextCmd(&offset[3]);
        gc_receiveOrNextCmd(&offset[2]);
        offset_u32 = ((offset[3] << 24) & 0xFF000000) | ((offset[2] << 16) & 0x00FF0000);
        gc_receiveOrNextCmd(&offset[1]);
        gc_receiveOrNextCmd(&offset[0]);
        dma_channel_start(DMA_WAIT_CHAN);

        //len -= 20;
        while (dma_channel_is_busy(DMA_WAIT_CHAN)); // Wait for DMA to complete
        gc_mc_respond(0xFF);


        //log(LOG_TRACE, "Unlock Msg4: Raw: %08x / %u\n", *(uint32_t*)offset, len);

        //log(LOG_TRACE, "Unlock Msg4: Unlock: %08x / %u\n", offset_u32, len);


        card_state = 0x41;
        unlock_stage = 0;
    }
    return;

}