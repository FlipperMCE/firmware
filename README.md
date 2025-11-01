<p align="center">
    <img src="doc/FlipperMCE.png" alt="FlipperMCE Logo" width="20%">
</p>

# FlipperMCE Firmware

FlipperMCE is a *MemoryCard Emulator* that is heavily based on sd2psx by developer @xyzz (see [here](https://github.com/sd2psx)).

## Features

It has the following base feature set:

- **Card Restore**
- **Game ID**
- **Multiple CardSizes**
- **Game2Folder mapping**
- **Settings File**
- **Per Card Configs**


### Card Restore

If active, FlipperMCE will always boot to the last used card.

### Game ID

Many loaders implement a special protocol, that allows the cube to transmit the Game ID of the currently started game to an *MCE*. FlipperMCE supports this protocol (see *doc/mcp*) and will load or create a card for each trasmitted Game ID if configured so.

### Mulitple CardSizes

*FlipperMCE* support Card Sizes from 4 - 64 MBits.


## Game2Folder mapping

There are some games, that share save data for multiple game ids. For these cases, a custom game to folder mapping can be created.

If a game with a mapped id is loaded, instead of using the game id based folder, the mapped folder is used for storing the card.

The mapping needs to be defined in ```.flippermce/Game2Folder.ini``` in the following way:

```ini
[GC]
GT4P=FolderName
```

*Note: Be aware: Long folder names may not be displayed correctly and may result in stuttering of MMCE games due to scrolling.*
*Note 2: Make sure there is an empty line at the end of the ini file.*


### Settings File

*FlipperMCE* generates a settings file (`.flippermce/settings.ini`) that allows you to edit some settings through your computer. This is useful when using one SD card with multiple *FlipperMCE* devices.

A settings file has the following format:

```ini
[General]
FlippedScreen=OFF
[GC]
CardRestore=ON
GameID=ON
CardSize=64
```

Possible values are:

| Setting       | Values                                |
|---------------|---------------------------------------|
| CardRestore   | `OFF`, `ON`                           |
| GameID        | `OFF`, `ON`                           |
| CardSize      | `4`, `8`, `16`, `32`, `64`            |
| FlippedScreen | `ON`, `OFF`                           |

*Note: Make sure there is an empty line at the end of the ini file.*

### Per Card Configs

There are some configuration values that can be modified on a per card base within a config file named  `CardX.ini` in a card folder, where `X` is the card index.

*Note: Make sure there is an empty line at the end of the ini file.*

```ini
[ChannelName]
1=Channel 1 Name
2=Channel 2 Name
3=Channel 3 Name
4=Channel 4 Name
5=Channel 5 Name
6=Channel 6 Name
7=Channel 7 Name
8=Channel 8 Name
[Settings]
MaxChannels=8
CardSize=8
```

### Card splashes

Add a splash image that the device shows in the card browser. Use the included splash generator (`misc/splashgen.html`) to convert a source image to the device `.bin` format, then place the generated file in the card folder on your SD card.

Naming and behavior
- Folder-level splash (default for the folder):
    - Path: `MemoryCards/GC/<card_folder>/<card_folder>.bin`
    - Example: `MemoryCards/GC/SuperGame/SuperGame.bin` — used when no channel-specific image exists.
- Channel-specific splash (overrides folder-level for that channel):
    - Path: `MemoryCards/GC/<card_folder>/<card_folder>-<channel_number>.bin`
    - Example: `MemoryCards/GC/SuperGame/SuperGame-1.bin` — shown only for channel 1 of that card folder.
- Fallback rules:
    - If a channel-specific file exists it is used.
    - Otherwise the folder-level `<card_folder>.bin` is used.
    - If neither exists, no splash is shown.

Practical notes
- `<channel_number>` matches the on-device channel index (1..N).
- Filenames must match exactly; FAT SD cards are usually case-insensitive but keep names consistent.
- Use `misc/splashgen.html` to produce correctly-sized and packed `.bin` files for the OLED.
- Keep names short — very long filenames may cause display or performance issues.
- Store splash `.bin` files alongside that card's `CardX.ini` and save data in the same folder.

## Known issues

### wii will not recognize the card as memory card

Within the wii System Menu, the card may occasionally not be recognized. If this happens please try the following things:

- Close and re-open Gamecube Cardbrowser
- Switch the mounted card using the buttons on FlipperMCE
- Re-Plug FlipperMCE

## Special Thanks to...

- **Vapor, rippenbiest, Mancloud**: for beta testing ❤️
- **@gameBitfunx**: for PCB design, testing and support ❤️
- **@xyz**: for sd2psx ❤️
- **sd2psXtd Team**: (you know who you are 😉 )
- **8BitMods Team**: for helping out with card formatting and providing lots of other useful information for things like unlock ❤️
- **@extrems**: For insights into EXI communications and libOGC2 SDK

