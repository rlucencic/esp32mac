/*
 * The ROMs this emulator knows, and the patches it makes to its copy.
 * Every patch checks the bytes it replaces first.
 *
 *   - The ROM checksum test is skipped (the copy is patched, so it would
 *     fail with a Sad Mac).
 *   - The power-on RAM test is shortened (Plus) or skipped (SE, Classic).
 *   - The .Sony disk driver is replaced by the paravirtual one (sony.c).
 *
 * Patch locations as in Mini vMac and umac, checked against these ROMs.
 */
#include <string.h>
#include "mac_internal.h"

typedef struct {
    uint32_t checksum;
    uint32_t size;
    const char *name;
    mac_model_t model;
} known_rom_t;

static const known_rom_t known[] = {
    { 0x4D1EEEE1, 0x20000, "Macintosh Plus (ROM v1)", MAC_MODEL_PLUS },
    { 0x4D1EEAE1, 0x20000, "Macintosh Plus (ROM v2)", MAC_MODEL_PLUS },
    { 0x4D1F8172, 0x20000, "Macintosh Plus (ROM v3)", MAC_MODEL_PLUS },
    { 0xB2E362A8, 0x40000, "Macintosh SE", MAC_MODEL_SE },
    { 0xB306E171, 0x40000, "Macintosh SE FDHD", MAC_MODEL_SE },
    { 0xA49F9914, 0x80000, "Macintosh Classic", MAC_MODEL_CLASSIC },
};

typedef struct {
    uint32_t offset;
    uint16_t expect;
    uint16_t value;
} patch_t;

static const patch_t plus_patches[] = {
    { 0x0D92, 0xB781, 0xB381 },     /* checksum: eor.l d3,d1 -> eor.l d1,d1 */
    { 0x0EA8, 0x6EEC, 0x4E71 },     /* RAM test read loop: nop */
    { 0x0E90, 0x6AF0, 0x4E71 },     /* RAM test write loop: nop */
};

static const patch_t se_patches[] = {
    { 0x1C68, 0x4EFA, 0x6008 },     /* checksum: branch past it */
    { 0x0086, 0x4EBA, 0x6002 },     /* RAM tests: skip the calls */
    { 0x011E, 0x4EBA, 0x6002 },
};

#define SONY_PLUS   0x17D30
#define SONY_SE     0x34680

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

const char *mac_rom_identify(const uint8_t *rom, uint32_t size, mac_model_t *model)
{
    if (rom == NULL || size < 4) {
        return NULL;
    }
    uint32_t sum = be32(rom);
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (known[i].checksum == sum && known[i].size == size) {
            if (model) {
                *model = known[i].model;
            }
            return known[i].name;
        }
    }
    return NULL;
}

/* The copy keeps each 16-bit word in host order */
static uint16_t rd16(uint32_t offset)
{
    return *(uint16_t *)(mac.rom + offset);
}

static void wr16(uint32_t offset, uint16_t value)
{
    *(uint16_t *)(mac.rom + offset) = value;
}

static void apply(const patch_t *patches, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint16_t now = rd16(patches[i].offset);
        if (now == patches[i].expect) {
            wr16(patches[i].offset, patches[i].value);
        } else {
            MAC_LOGW("ROM patch at $%05lx skipped: $%04x, expected $%04x",
                     (unsigned long)patches[i].offset, now, patches[i].expect);
        }
    }
}

bool rom_prepare(const uint8_t *image, uint32_t size)
{
    /* Byte pairs swapped (works in place too: image may be the copy) */
    for (uint32_t i = 0; i < size; i += 2) {
        uint8_t hi = image[i], lo = image[i + 1];
        mac.rom[i] = lo;
        mac.rom[i + 1] = hi;
    }
    uint32_t sony;
    if (mac.model == MAC_MODEL_PLUS) {
        apply(plus_patches, sizeof(plus_patches) / sizeof(plus_patches[0]));
        sony = SONY_PLUS;
    } else {
        apply(se_patches, sizeof(se_patches) / sizeof(se_patches[0]));
        sony = SONY_SE;
    }
    /* The original driver: flags $4F00 and the name ".Sony" */
    static const uint8_t name[6] = { 5, '.', 'S', 'o', 'n', 'y' };
    bool found = rd16(sony) == 0x4F00;
    for (int i = 0; i < 6 && found; i++) {
        found = mac.rom[(sony + 0x12 + i) ^ 1] == name[i];
    }
    if (!found) {
        MAC_LOGW("no .Sony driver at $%05lx", (unsigned long)sony);
        return false;
    }
    return sony_install(mac.rom, size, sony);
}
