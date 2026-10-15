#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "mac.h"

#define ROM_MAX_SIZE    (512 * 1024)

/* Load the ROM for `model` into buf (ROM_MAX_SIZE bytes): from its flash
 * partition (rom_plus, rom_se, rom_classic: roms/<model>.rom, written by
 * idf.py flash), else from a file in the SD card's "roms" folder whose
 * checksum is a ROM of that model. Its size in *size, its name in *name.
 */
bool rom_store_load(mac_model_t model, uint8_t *buf, uint32_t *size, const char **name);

/* Which models have a ROM (bit n = model n), for the setup screen */
uint8_t rom_store_available(void);
