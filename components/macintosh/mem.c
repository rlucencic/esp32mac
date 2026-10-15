/*
 * Memory map and the Musashi bus callbacks (24-bit addresses, 1 MB regions):
 *
 *   $000000-$3FFFFF  RAM, repeated every ram_size; ROM while the overlay is on
 *   $400000-$4FFFFF  ROM, repeated every rom_size (SE, Classic: the first
 *                    access here turns the overlay off)
 *   $580000-$5FFFFF  SCSI 5380 (reads on even, writes on odd addresses)
 *   $600000-$7FFFFF  RAM (where it is while the overlay is on)
 *   $800000-$9FFFFF  SCC reads ($9FFFF8 + ...)
 *   $A00000-$BFFFFF  SCC writes ($BFFFF9 + ...)
 *   $C00000-$DFFFFF  IWM ($DFE1FF + register * $200), odd bytes
 *   $E00000-$EFFFFF  VIA ($EFE1FE + register * $200), even bytes
 *   $F00000-$FFFFFF  nothing (reads 0)
 */
#include "m68k.h"
#include "mac_internal.h"

static inline uint16_t rd16(const uint8_t *mem, uint32_t offset)
{
    return *(const uint16_t *)(mem + offset);
}

static inline void wr16(uint8_t *mem, uint32_t offset, uint16_t value)
{
    *(uint16_t *)(mem + offset) = value;
}

static uint8_t io_read8(uint32_t a)
{
    switch (a >> 20) {
    case 0x5:
        return a >= 0x580000 ? scsi_read(a) : 0;
    case 0x8: case 0x9:
        return (a & 1) ? 0 : scc_read(a);
    case 0xC: case 0xD:
        return (a & 1) ? iwm_access(a, false, 0) : 0xFF;
    case 0xE:
        return (a & 1) ? 0 : via_read(a);
    default:
        return 0;
    }
}

static void io_write8(uint32_t a, uint8_t v)
{
    switch (a >> 20) {
    case 0x5:
        if (a >= 0x580000) {
            scsi_write(a, v);
        }
        break;
    case 0xA: case 0xB:
        if (a & 1) {
            scc_write(a, v);
        }
        break;
    case 0xC: case 0xD:
        if (a & 1) {
            iwm_access(a, true, v);
        }
        break;
    case 0xE:
        if (!(a & 1)) {
            via_write(a, v);
        }
        break;
    default:
        break;
    }
}

/* ROM region: on the SE and Classic the first access ends the overlay */
static inline uint32_t rom_offset(uint32_t a)
{
    if (mac.overlay && mac.adb) {
        mac_set_overlay(false);
    }
    return a & mac.rom_mask;
}

static uint8_t slow_read8(uint32_t a)
{
    switch (a >> 20) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        return mac.overlay ? mac.rom[(a & mac.rom_mask) ^ 1] : mac.ram[(a & mac.ram_mask) ^ 1];
    case 0x4:
        return mac.rom[rom_offset(a) ^ 1];
    case 0x6: case 0x7:
        return mac.ram[(a & mac.ram_mask) ^ 1];
    default:
        return io_read8(a);
    }
}

static uint16_t slow_read16(uint32_t a)
{
    switch (a >> 20) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        return mac.overlay ? rd16(mac.rom, a & mac.rom_mask) : rd16(mac.ram, a & mac.ram_mask);
    case 0x4:
        return rd16(mac.rom, rom_offset(a));
    case 0x6: case 0x7:
        return rd16(mac.ram, a & mac.ram_mask);
    default: {
        uint16_t hi = io_read8(a);
        return (hi << 8) | io_read8(a + 1);
    }
    }
}

static void slow_write8(uint32_t a, uint8_t v)
{
    switch (a >> 20) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        if (!mac.overlay) {
            mac.ram[(a & mac.ram_mask) ^ 1] = v;
        }
        break;
    case 0x4:
        rom_offset(a);
        break;
    case 0x6: case 0x7:
        mac.ram[(a & mac.ram_mask) ^ 1] = v;
        break;
    default:
        io_write8(a, v);
        break;
    }
}

static void slow_write16(uint32_t a, uint16_t v)
{
    switch (a >> 20) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        if (!mac.overlay) {
            wr16(mac.ram, a & mac.ram_mask, v);
        }
        break;
    case 0x4:
        rom_offset(a);
        break;
    case 0x6: case 0x7:
        wr16(mac.ram, a & mac.ram_mask, v);
        break;
    default:
        io_write8(a, v >> 8);
        io_write8(a + 1, v & 0xFF);
        break;
    }
}

/* ---- Musashi callbacks: fast paths for RAM and ROM ---- */

unsigned int MAC_HOT m68k_read_memory_8(unsigned int a)
{
    if (a < mac.ram_window) {
        return mac.ram[(a & mac.ram_mask) ^ 1];
    }
    return slow_read8(a);
}

unsigned int MAC_HOT m68k_read_memory_16(unsigned int a)
{
    if (a < mac.ram_window) {
        return rd16(mac.ram, a & mac.ram_mask);
    }
    if ((a & 0xF00000) == 0x400000 && !mac.overlay) {
        return rd16(mac.rom, a & mac.rom_mask);
    }
    return slow_read16(a);
}

unsigned int MAC_HOT m68k_read_memory_32(unsigned int a)
{
    if (a + 2 < mac.ram_window) {
        uint32_t o = a & mac.ram_mask;
        if (o <= mac.ram_mask - 3) {
            return ((uint32_t)rd16(mac.ram, o) << 16) | rd16(mac.ram, o + 2);
        }
    }
    if ((a & 0xF00000) == 0x400000 && !mac.overlay) {
        uint32_t o = a & mac.rom_mask;
        if (o <= mac.rom_mask - 3) {
            return ((uint32_t)rd16(mac.rom, o) << 16) | rd16(mac.rom, o + 2);
        }
    }
    uint32_t hi = slow_read16(a);
    return (hi << 16) | slow_read16((a + 2) & 0xFFFFFF);
}

void MAC_HOT m68k_write_memory_8(unsigned int a, unsigned int v)
{
    if (a < mac.ram_window) {
        mac.ram[(a & mac.ram_mask) ^ 1] = v;
        return;
    }
    slow_write8(a, v);
}

void MAC_HOT m68k_write_memory_16(unsigned int a, unsigned int v)
{
    if (a < mac.ram_window) {
        wr16(mac.ram, a & mac.ram_mask, v);
        return;
    }
    slow_write16(a, v);
}

void MAC_HOT m68k_write_memory_32(unsigned int a, unsigned int v)
{
    if (a + 2 < mac.ram_window) {
        uint32_t o = a & mac.ram_mask;
        if (o <= mac.ram_mask - 3) {
            wr16(mac.ram, o, v >> 16);
            wr16(mac.ram, o + 2, v & 0xFFFF);
            return;
        }
    }
    slow_write16(a, v >> 16);
    slow_write16((a + 2) & 0xFFFFFF, v & 0xFFFF);
}

/* ---- Side-effect-free access for the emulator ---- */

uint16_t mac_peek16(uint32_t a)
{
    a &= 0xFFFFFE;
    switch (a >> 20) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        return mac.overlay ? rd16(mac.rom, a & mac.rom_mask) : rd16(mac.ram, a & mac.ram_mask);
    case 0x4:
        return rd16(mac.rom, a & mac.rom_mask);
    case 0x6: case 0x7:
        return rd16(mac.ram, a & mac.ram_mask);
    default:
        return 0;
    }
}

uint8_t mac_peek8(uint32_t a)
{
    uint16_t w = mac_peek16(a & ~1u);
    return (a & 1) ? (w & 0xFF) : (w >> 8);
}

uint32_t mac_peek32(uint32_t a)
{
    return ((uint32_t)mac_peek16(a) << 16) | mac_peek16(a + 2);
}

/* Writes go to RAM only (low memory and the system heap: below ram_size) */
void mac_poke8(uint32_t a, uint8_t v)
{
    if (a < mac.ram_size) {
        mac.ram[a ^ 1] = v;
    }
}

void mac_poke16(uint32_t a, uint16_t v)
{
    if (a + 1 < mac.ram_size) {
        wr16(mac.ram, a & ~1u, v);
    }
}

void mac_poke32(uint32_t a, uint32_t v)
{
    mac_poke16(a, v >> 16);
    mac_poke16(a + 2, v & 0xFFFF);
}

bool mac_ram_range(uint32_t a, uint32_t len)
{
    return a < mac.ram_size && len <= mac.ram_size - a;
}

void mac_ram_write(uint32_t a, const uint8_t *src, uint32_t len)
{
    if ((a & 1) == 0) {
        /* Whole words: swap pairs */
        uint32_t i = 0;
        for (; i + 1 < len; i += 2) {
            mac.ram[a + i] = src[i + 1];
            mac.ram[a + i + 1] = src[i];
        }
        if (i < len) {
            mac.ram[(a + i) ^ 1] = src[i];
        }
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        mac.ram[(a + i) ^ 1] = src[i];
    }
}

void mac_ram_read(uint32_t a, uint8_t *dst, uint32_t len)
{
    if ((a & 1) == 0) {
        uint32_t i = 0;
        for (; i + 1 < len; i += 2) {
            dst[i] = mac.ram[a + i + 1];
            dst[i + 1] = mac.ram[a + i];
        }
        if (i < len) {
            dst[i] = mac.ram[(a + i) ^ 1];
        }
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = mac.ram[(a + i) ^ 1];
    }
}

unsigned int m68k_read_disassembler_16(unsigned int a)
{
    return mac_peek16(a);
}

unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return mac_peek32(a);
}

/* ---- Free memory, from the Memory Manager's zone headers ---- */

#define LM_BUFPTR       0x010C
#define LM_HEAPEND      0x0114
#define LM_APPLLIMIT    0x0130
#define LM_SYSZONE      0x02A6
#define LM_APPLZONE     0x02AA
#define ZONE_BKLIM      0       /* end of the zone */
#define ZONE_ZCBFREE    12      /* free bytes in it */
#define ZONE_HEADER     52

static uint32_t ptr24(uint32_t a)
{
    return mac_peek32(a) & 0xFFFFFF;
}

/* A zone header at z ending at or below `top`: its free bytes, or -1 */
static int64_t zone_free(uint32_t z, uint32_t top)
{
    if (z == 0 || (z & 1) || z + ZONE_HEADER >= mac.ram_size) {
        return -1;
    }
    uint32_t lim = ptr24(z + ZONE_BKLIM);
    uint32_t free_bytes = mac_peek32(z + ZONE_ZCBFREE);
    if (lim <= z + ZONE_HEADER || lim > top || free_bytes >= lim - z) {
        return -1;
    }
    return free_bytes;
}

bool mac_ram_free(uint32_t *free_bytes, uint32_t *system_free)
{
    *free_bytes = *system_free = 0;
    if (mac.ram == NULL || mac.overlay || mac_peek32(LM_TICKS) == 0) {
        return false;
    }
    uint32_t top = ptr24(LM_BUFPTR);
    uint32_t sys = ptr24(LM_SYSZONE);
    if (top == 0 || top > mac.ram_size) {
        return false;
    }
    int64_t sys_free = zone_free(sys, top);
    if (sys_free < 0) {
        return false;
    }
    *system_free = sys_free;

    /* System 7: the Process Manager's heap follows the system heap (after
     * its 12-byte end block) and reaches up to just below BufPtr
     */
    uint32_t sys_end = ptr24(sys + ZONE_BKLIM);
    for (uint32_t z = sys_end; z <= sys_end + 64; z += 2) {
        int64_t f = zone_free(z, top);
        if (f >= 0 && ptr24(z + ZONE_BKLIM) + 0x10000 >= top) {
            *free_bytes = f;
            return true;
        }
    }

    /* Otherwise: the application heap, and the room it can still grow into */
    uint32_t app = ptr24(LM_APPLZONE);
    int64_t app_free = zone_free(app, top);
    if (app_free < 0) {
        return false;
    }
    uint32_t heap_end = ptr24(LM_HEAPEND), limit = ptr24(LM_APPLLIMIT);
    *free_bytes = app_free + (limit > heap_end && limit <= top ? limit - heap_end : 0);
    return true;
}
