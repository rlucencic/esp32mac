/*
 * Compact Macintosh: Mac Plus, SE and Classic (68000, 512x342 black and
 * white). Musashi 68000, VIA 6522 with timers, RTC with parameter RAM,
 * Plus keyboard or ADB keyboard and mouse, minimal SCC/IWM/SCSI, the screen
 * and sound buffers. Disk images (any size, floppy or hard disk) go through
 * a paravirtual replacement of the ROM's .Sony driver.
 *
 * Platform independent: the caller provides all memory and drives the
 * emulation one video frame at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MAC_MODEL_PLUS,         /* 128 KB ROM, keyboard on the VIA shift register */
    MAC_MODEL_SE,           /* 256 KB ROM, ADB */
    MAC_MODEL_CLASSIC,      /* 512 KB ROM (with the ROM disk), ADB */
} mac_model_t;

#define MAC_SCREEN_WIDTH        512
#define MAC_SCREEN_HEIGHT       342
#define MAC_SCREEN_HZ           60

#define MAC_DRIVES              3       /* Mac drive numbers 1 .. 3 */
#define MAC_PRAM_SIZE           256     /* parameter RAM, incl. extended */
#define MAC_OPCODE_TABLES_SIZE  0x50000

typedef struct {
    uint8_t *ram;           /* ram_size bytes */
    uint32_t ram_size;      /* 1, 2 or 4 MB */
    const uint8_t *rom;     /* ROM image as dumped (big-endian) */
    uint32_t rom_size;      /* 128, 256 or 512 KB */
    uint8_t *rom_copy;      /* rom_size bytes: working copy (it gets patched);
                             * may be the same buffer as rom */

    /* MAC_PRAM_SIZE bytes of parameter RAM, kept by the caller across
     * power-offs. The Mac writes into it; pram_changed is called (from the
     * emulation task) after it did, at most once a frame.
     */
    uint8_t *pram;
    void (*pram_changed)(void);

    /* The Mac ejected the disk in this drive (0-based), from the emulation
     * task: the file is closed already
     */
    void (*disk_ejected)(int drive);

    /* MAC_OPCODE_TABLES_SIZE bytes for copies of the CPU core's opcode
     * tables, or NULL (see st.h of the Atari: read-only data in flash is slow)
     */
    void *opcode_tables;
} mac_config_t;

typedef struct {
    uint32_t frames;        /* video frames emulated */
    uint32_t pc;
    uint16_t sr;
    uint32_t ticks;         /* Mac OS Ticks (0 before the OS is up) */
    bool halted;            /* CPU stopped by a double bus fault until a reset */
} mac_status_t;

/* Fill MAC_PRAM_SIZE bytes of parameter RAM with defaults (mac_init does
 * it for parameter RAM that isn't valid)
 */
void mac_pram_defaults(uint8_t *pram, mac_model_t model);

/* Name of a known ROM ("Macintosh Plus v3" ...) and its model; NULL if the
 * image isn't a ROM this emulator knows how to patch
 */
const char *mac_rom_identify(const uint8_t *rom, uint32_t size, mac_model_t *model);

/* Set up the machine and cold-reset it. False if the ROM is unusable. */
bool mac_init(const mac_config_t *config);

/* Cold reset, as the power switch */
void mac_reset(void);

/* Programmer's interrupt switch (NMI): the debugger, if one is installed */
void mac_interrupt_switch(void);

/* Emulate one video frame (60.15 Hz) */
void mac_run_frame(void);

void mac_get_status(mac_status_t *status);
mac_model_t mac_model(void);

/* Free RAM as the Mac OS sees it, in bytes: the Process Manager's heap
 * under System 7 ("Largest Unused Block" territory), else the application
 * heap and the room above it; and the system heap's free bytes. False
 * before the OS is up. Safe to call from another task.
 */
bool mac_ram_free(uint32_t *free_bytes, uint32_t *system_free);

/* Read Mac memory without side effects (RAM and ROM only, else 0) */
uint16_t mac_peek16(uint32_t address);

/* ---- Screen: 512 x 342, one bit per pixel ---- */

/* Render screen line y (0 .. 341) as RGB565, 512 pixels */
void mac_render_line_rgb565(int y, uint16_t *dst);

/* Hash of the visible screen buffer: changes when the picture changes */
uint32_t mac_screen_signature(void);

/* ---- Sound: 16-bit stereo at MAC_AUDIO_HZ (one sample per scanline) ---- */

#define MAC_AUDIO_HZ            22255

void mac_audio_enable(bool enable);
int mac_audio_render(int16_t *dst, int frames);
int mac_audio_pending(void);

/* ---- Disk images: drives 0 .. MAC_DRIVES - 1 (Mac drive numbers 1 ..) ----
 *
 * Raw images (.dsk, .img, .hfv: 400K, 800K, 1.4M floppies or hard disks of
 * any size) and Disk Copy 4.2 images (.image, read and written in place).
 * A read-only file is a locked disk. Not while mac_run_frame() runs; the
 * Mac sees the disk inserted within half a second.
 */
bool mac_disk_insert(int drive, const char *path);
void mac_disk_eject(int drive);
const char *mac_disk_path(int drive);      /* NULL when empty */
bool mac_disk_write_protected(int drive);

/* Size in bytes of the disk in the drive, 0 when empty */
uint64_t mac_disk_size(int drive);

/* The free space on the disk in the drive and its volume's size, as its
 * MFS, HFS or HFS+ volume counts them (worked out at insert and a second
 * after the last write); false if not known. Safe to call from another task.
 */
bool mac_disk_space(int drive, uint64_t *free_bytes, uint64_t *size);

/* Make a blank disk image of `bytes` (a multiple of 512): the Mac offers to
 * initialise it when it's inserted. Only the first and last 64 KB (boot
 * blocks, volume headers) are written as zeros; the rest is whatever the
 * file system gives a grown file, which initialising overwrites anyway.
 */
bool mac_disk_create(const char *path, uint32_t bytes);

typedef struct {
    int light;              /* drive accessed in the last ~0.3 s, -1 none */
    uint32_t reads[MAC_DRIVES];     /* 512-byte blocks read and written */
    uint32_t writes[MAC_DRIVES];
} mac_disk_status_t;

void mac_disk_status(mac_disk_status_t *status);

/* ---- Keyboard and mouse: safe to call from another task ---- */

/* Mac virtual key code (ADB key code, e.g. 0x00 = A, 0x37 = Command,
 * 0x7B..0x7E = arrows). Plus keyboards get the matching transitions.
 */
void mac_key(uint8_t keycode, bool down);

/* Relative mouse motion (pixels) and the button */
void mac_mouse(int dx, int dy, bool button);

/* Put the pointer at screen position (x, y) (the console, host scripts) */
void mac_mouse_position(int x, int y, bool button);

/* The RTC follows the host's local time: set the system clock and TZ and
 * the Mac has the time. Times the Mac sets itself are kept as an offset.
 */
