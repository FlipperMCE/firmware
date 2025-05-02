# Unlock

The unlock sequence is required to maintain compatibility with  all games. It was required for both the cube authenticating the card, and the card authenticating the cube.

## Pre-Requisites

### Initialization

During initialization, there are multiple messages exchanged between the card and the cube.

1. **0x00 0x00 - Probe:** Read basic information from the card, encoded into 2 bytes. There is a couple of data ecnoded into this

    Information | Description
    ------------|---------------------------
    Size        | Card Size in MegaBits
    Latency     | Number of Latency Cycles the card needs to do a read operation.
    Sector Size | Sector Size in Bytes

    This information needs to be encoded in the following way:
    ```c
    static void mc_generateId(void) {
        uint32_t size = 8 * GC_MC_MB_CARD_SIZE;
        uint32_t latency = __builtin_ctz(GC_MC_LATENCY_CYCLES) - __builtin_ctz(0x4);
        uint32_t sector_size = __builtin_ctz(GC_MC_SECTOR_SIZE) - __builtin_ctz(0x2000);
        uint32_t value = (size & 0xfc) |
            ((_ROTL(latency << 2, 6))) |
            ((_ROTL(sector_size << 2, 9)));
        mc_probe_id[0] = (value >> 8) & 0xFF;
        mc_probe_id[1] = value & 0xFF;
    }
    ```

2. **0x89 - ??:** Do nothing

3. **0x83 - Get card state**: Read current card state. The state has several bits that can be set. Most relevant for unlock is Bit 3 / 0x40. Before Unlock, the card replies 0x81 or 0x01. After Unlock it is supposed to reply 0x41. This is the third byte in the transfer.

4. **0x81 - Clear Card State:** The card state is cleared to have a clean initialization. No need to respond.

After these initial commands, the unlock process is started.


## Cheating

It is possible to trick the cube in thinking an unlock has already been done by OR‘ing the state of the card with 0x40 without prior unlock. This is done by many third party cards, but has the following drawbacks:
1. The card needs to be formatted every time a legit card has been attached
2. For games that require unlock, this technique simply doesn’t work

## Real unlock

During real unlock, the cube requests the scrambled serial number from the card, which is then stored in the flash_id field for this cardslot in SRAM of the cube.
Once the initial unlock sequence has finished, the cube checks, whether the card serial stored in SRAM does match the serial format and if the serial is matching to the serial that is read in plaintext later. If this is not the case, the card is rejected.

The real unlock sequence consists of 4 parts:

### Part 1: Initial Cipher transfer

The cube is first generating a cipher using the following pseudo random values:

- Random address between 0x7FEC8000 and 0x7FEFFFFF
- Random message length between 4 and 32

This cipher is supposed to use as a starting point for tranforming the serial number in 4 byte blocks.

The random address is transferrred to the cube by applying the following bitshifting:
```c
    message[0] = 0x52;
    message[1] = ((address&0x60000000)>>29)&0xff;
    message[2] = ((address&0x1FE00000)>>21)&0xff;
    message[3] = ((address&0x00180000)>>19)&0xff;
    message[4] = ((address&0x0007F000)>>12)&0xff;
```

In addition to the address, there is the dummy latency transfer (depending on the card latency) and additionally the message length that was created randomly.

As an example, the length of the total message for a card with 256 latency bytes and a random length of 8 is:
```
5 Byte payload + 256 Byte Latency + 8 Byte Random length = 269 Bytes
```

After the first transfer, the cipher needs to be transformed using the following code (heavily leaning on libOGC):

```c
uint32_t initCipher(uint32_t address, uint8_t length)
{
    uint32_t cipher = 0;
    for (uint8_t i = 0; i < length; i++)
    {
        uint32_t d,e,f,r1,r2,r3,r4;
        d = (address>>23);
        e = (address>>15);
        f = (address>>7);
        r1 = (address^f);
        r2 = (e^r1);
        r3 = ~(d^r2);		//eqv(d,r2)
        e = (address>>1);
        r4 = ((r3<<30)&0x40000000);
        address = (e|r4);
    }
    {
        uint32_t a,b,c,r1,r2,r3;
        a = (address>>23);
        b = (address>>15);
        c = (address>>7);
        r1 = (address^c);
        r2 = (b^r1);
        r3 = ~(a^r2);		//eqv(a,r2)
        r1 = (address|(r3<<31));
        cipher = r1;
    }
    return bitrev(cipher);
}
```

### Part 2: Transformed serial transfer

The Gamecube requests the flash id of the card encoded in the serial. The flash id can be extracted from the serial and the format time stamp in the header of the card.

*Note: The time is stored at offset 12 in big endian format, so depending on the architecture, this needs to be converted!*

```c
static void extract_flash_id(uint8_t *flash_id, uint8_t *chksum, uint8_t *serial, uint64_t time)
{
    // Create flash_id from base_serial
    uint64_t rand = time;

    for (int i = 0; i < 12; i++)
    {
      rand = (((rand * (uint64_t)0x0000000041c64e6dULL) + (uint64_t)0x0000000000003039ULL) >> 16);
      *chksum += flash_id[i] = serial[i] - ((uint8_t)rand & 0xff);
      rand = (((rand * (uint64_t)0x0000000041c64e6dULL) + (uint64_t)0x0000000000003039ULL) >> 16);
      rand &= (uint64_t)0x0000000000007fffULL;
    }
    *chksum = *chksum ^ 0xff;
}
```

The **flash id** now needs to be XORed in 4 Byte blocks. Since the flash id is also in big endian format, it needs to be converted before the XOR operation if calculated on little endian machines.

After each XOR, the cipher needs to be updated to its next state:

```c
void update_cipher(uint32_t* cipher, uint32_t count) {
    uint32_t val = *cipher;
    for (uint8_t i = 0; i < count; i++)
    {
        uint32_t d,e,f,r1,r2,r3,r4;
        d = (val<<23);
        e = (val<<15);
        f = (val<<7);
        r1 = (val^f);
        r2 = (e^r1);
        r3 = ~(d^r2);		//eqv(d,r2)
        e = (val<<1);
        r4 = ((r3>>30)&0x02);
        val = (e|r4);
    }
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
```

On a little endian machine, the code for generating the first 12 response bytes can look like this:

```c
    uint8_t response[20] = {};
    *(uint32_t *)&response[0] = ((uint32_t*)flash_id)[0] ^ swap32(card_cipher);
    update_cipher(&card_cipher, 32);
    *(uint32_t *)&response[4] = ((uint32_t*)flash_id)[1] ^ swap32(card_cipher);
    update_cipher(&card_cipher, 32);
    *(uint32_t *)&response[8] = ((uint32_t*)flash_id)[2] ^ swap32(card_cipher);
```

The next 8 bytes of the response are used as a challenge to the cube which is supposed to do some hashing on DSP and send the result as address bytes for the next read.

Since its enough for us to simulate the card, we can basically go with every challenge we like.

```c
    uint8_t response[20] = {};
    *(uint32_t *)&response[0] = 0x00 ^ swap32(card_cipher);
    update_cipher(&card_cipher, 32);
    *(uint32_t *)&response[4] = 0x00 ^ swap32(card_cipher);
    update_cipher(&card_cipher, 32);
    *(uint32_t *)&response[8] = 0x00 ^ swap32(card_cipher);
```


### Part 3 / 4: Challenge response

Since we are blindly accepting every device that wants to talk to us, we just return 0xFFs to the next two messages.

**After message 4 we need to make sure to set the unlocked flag, since otherwise the cube may not be able to read our output.**
