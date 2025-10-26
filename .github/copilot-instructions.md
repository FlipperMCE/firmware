# FlipperMCE Development Guide

This guide helps AI agents understand the essential patterns and workflows for contributing to FlipperMCE, a MemoryCard Emulator based on sd2psx.

## Project Overview

FlipperMCE is a memory card emulator firmware targeting RP2040-based hardware. Key components:

- Core emulation logic in `src/gc/gc_cardman.c` and related files
- UI/display handling via OLED screen (`src/oled.c`, `src/gui.c`)
- Settings management and configuration parsing (`src/settings.c`)
- Hardware interfaces for GPIO, PSRAM, and SD card access
- Game database handling (`database/` directory)

## Development Environment

### Build System
- Uses CMake with Pico SDK
- Key files:
  - `CMakeLists.txt` - Main build configuration
  - `pico_sdk_import.cmake` - Pico SDK integration
  - `memmap_custom.ld` - Custom memory layout

### Hardware Requirements
- RP2040-based development board
- OLED display
- PSRAM chip
- SD card interface

## Key Development Workflows

### Building
1. Ensure Pico SDK is properly set up
2. Configure cmake with appropriate target variant
3. Build produces `.uf2` files in `build/` directory

### Flashing
- Hold either button while connecting device to enter bootloader mode
- Copy `.uf2` file to mounted RPI-RP2 volume (see `flasher/` tools)

### Configuration
- Settings stored in `.flippermce/settings.ini` on SD card
- Per-card configs in `CardX.ini` files

## Critical Patterns

### Memory Card Emulation
- Card data stored in PSRAM and backed by SD card
- Game ID detection protocol documented in `doc/mcp/`
- Multiple card sizes supported (4-64 MBit)

### Hardware Interface
- Core 0: Main UI and management
- Core 1: Time-critical EXI protocol handling
- Uses DMA for efficient memory transfers

### Game Database
- Pre-parsed during build (`database/parse_*.py`)
- Supports multiple console formats
- Game-to-folder mapping via `Game2Folder.ini`

## Common Tasks

### Adding New Game Support
1. Update relevant database parser in `database/`
2. Test with both stock and Game ID-based card creation
3. Verify proper card size selection

### UI Changes
1. Modify `src/gui.c` and related files
2. Test across different screen orientations (FlippedScreen setting)
3. Ensure memory-efficient rendering

### Debugging
- UART debug output available (see `debug.c`)
- USB serial commands for testing:
  - `blr` - Reset to bootloader
  - `rrr` - Soft reset
  - `ch+/ch-` - Change channel

## Integration Points

- EXI protocol handling in `src/gc/`
- LVGL UI framework integration
- SD card filesystem access via ESP8266SdFat
- External dependencies in `ext/`