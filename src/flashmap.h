#pragma once

/* application, including rp2040 bootloader */
#define FLASH_OFF_APP (0x0)

/* 4k before eeprom starts */
#ifndef FLASH_OFF_SPLASH
    #define FLASH_OFF_SPLASH (0x7fb000)
#endif

/* 16k space before 8MB boundary */
#ifndef FLASH_OFF_EEPROM
    #define FLASH_OFF_EEPROM (0x7fc000)
#endif

/* at the 8MB boundary */
#define FLASH_OFF_GCEXP (0x800000)
