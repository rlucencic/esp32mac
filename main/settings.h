#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "mac.h"

/* Machine settings kept in NVS (namespace "mac") */
typedef struct {
    uint8_t model;          /* mac_model_t: which ROM to start */
    uint16_t ram_kb;        /* 1024, 2048, 4096 */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);

/* Parameter RAM (MAC_PRAM_SIZE bytes): false if none was saved yet */
bool settings_load_pram(uint8_t *pram);
void settings_save_pram(const uint8_t *pram);
