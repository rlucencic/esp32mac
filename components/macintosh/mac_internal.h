/*
 * Shared state of the Macintosh. Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "mac.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_log.h"
#define MAC_HOT             IRAM_ATTR
#define MAC_LOGI(...)       ESP_LOGI("mac", __VA_ARGS__)
#define MAC_LOGW(...)       ESP_LOGW("mac", __VA_ARGS__)
#else
#include <stdio.h>
#define MAC_HOT
#define MAC_LOGI(fmt, ...)  printf("I mac: " fmt "\n", ##__VA_ARGS__)
#define MAC_LOGW(fmt, ...)  printf("W mac: " fmt "\n", ##__VA_ARGS__)
#endif

/* 15.6672 MHz / 2. A scanline is 704 pixel clocks (352 CPU cycles), a frame
 * 370 lines (342 visible), 60.15 Hz.
 */
#define MAC_CPU_HZ          7833600
#define LINE_CYCLES         352
#define FRAME_LINES         370
#define FRAME_CYCLES        (LINE_CYCLES * FRAME_LINES)
#define VISIBLE_CYCLES      256         /* 512 pixels of a line */
#define VIA_DIVIDER         10          /* VIA clock: CPU / 10 */

/* Screen and sound buffers: below the top of RAM, page 2 further down */
#define SCREEN_OFFSET       0x5900
#define SCREEN_OFFSET_ALT   0xD900
#define SCREEN_BYTES        (MAC_SCREEN_WIDTH / 8 * MAC_SCREEN_HEIGHT)
#define SOUND_OFFSET        0x0300
#define SOUND_OFFSET_ALT    0x5F00

/* Low memory globals */
#define LM_TICKS            0x016A
#define LM_MTEMP            0x0828
#define LM_RAWMOUSE         0x082C
#define LM_MOUSE            0x0830
#define LM_CRSRNEW          0x08CE
#define LM_CRSRCOUPLE       0x08CF
#define LM_JIODONE          0x08FC

/* Scheduled events (cycle times) */
enum {
    EV_VIA_T1,
    EV_VIA_T2,
    EV_KBD,             /* Plus keyboard answers a command */
    EV_ADB,             /* ADB transceiver acts on a new state */
    EV_COUNT,
};

#define EV_NEVER            UINT64_MAX

typedef struct {
    mac_model_t model;
    bool adb;                       /* SE / Classic */

    /* Memory. RAM and ROM are stored with each 16-bit word in host order,
     * so a word read is a plain load and byte n lives at index n ^ 1.
     */
    uint8_t *ram;
    uint32_t ram_size;
    uint32_t ram_mask;
    uint8_t *rom;
    uint32_t rom_size;
    uint32_t rom_mask;
    bool overlay;                   /* ROM at 0, RAM at $600000 (power-on) */
    uint32_t ram_window;            /* RAM at 0 .. ram_window: 0 with overlay */
    uint32_t driver_start;          /* the replacement .Sony driver in the ROM */
    uint32_t driver_end;            /* (Mac addresses, native calls only there) */

    /* Time */
    uint64_t now;                   /* CPU cycles, as of the last sync */
    uint64_t frame_start;
    bool cpu_running;               /* inside m68k_execute() */
    int slice;                      /* cycles asked of m68k_execute() */
    uint64_t events[EV_COUNT];
    uint64_t next_second;           /* RTC one-second interrupt */

    uint32_t frames;
    bool halted;

    /* Interrupt lines */
    bool via_irq;
    bool scc_irq;
    bool nmi;

    uint8_t *pram;
    void (*pram_changed)(void);
    bool pram_dirty;
    void (*disk_ejected)(int drive);
} mac_machine_t;

extern mac_machine_t mac;

/* mac.c */
uint64_t mac_now(void);
void mac_schedule(int event, uint64_t when);
void mac_update_irq(void);
void mac_set_overlay(bool on);
uint32_t mac_ram_top(void);         /* ram_size, the end of RAM as the Mac sees it */

/* mem.c: memory access for the emulator itself (no side effects) */
uint8_t mac_peek8(uint32_t address);
uint32_t mac_peek32(uint32_t address);
void mac_poke8(uint32_t address, uint8_t value);
void mac_poke16(uint32_t address, uint16_t value);
void mac_poke32(uint32_t address, uint32_t value);
bool mac_ram_range(uint32_t address, uint32_t len);
void mac_ram_write(uint32_t address, const uint8_t *src, uint32_t len);
void mac_ram_read(uint32_t address, uint8_t *dst, uint32_t len);

/* via.c */
void via_reset(void);
uint8_t via_read(uint32_t address);
void via_write(uint32_t address, uint8_t value);
void via_timer_event(int event);
void via_set_flag(int flag);        /* VIA_IF_* */
void via_vbl(void);
void via_one_second(void);
uint8_t via_port_a(void);           /* output levels as the Mac set them */
uint8_t via_port_b(void);
/* Shift register, as the external device (keyboard, ADB transceiver) */
bool via_sr_shifting_in(void);      /* ACR: shift in under external clock */
bool via_sr_shifting_out(void);     /* ACR: shift out under external clock */
void via_sr_receive(uint8_t value); /* device -> Mac: SR full, IRQ */
uint8_t via_sr_send(void);          /* Mac -> device: SR emptied, IRQ */

#define VIA_IF_CA2      0x01        /* one-second interrupt */
#define VIA_IF_CA1      0x02        /* vertical blanking */
#define VIA_IF_SR       0x04        /* keyboard / ADB data */
#define VIA_IF_CB2      0x08
#define VIA_IF_CB1      0x10
#define VIA_IF_T2       0x20
#define VIA_IF_T1       0x40

/* Port bits */
#define PA_SOUND_VOL    0x07
#define PA_SOUND_PAGE   0x08        /* 1 = main sound buffer */
#define PA_OVERLAY      0x10        /* Plus: ROM overlay */
#define PA_HEAD_SEL     0x20
#define PA_SCREEN_PAGE  0x40        /* 1 = main screen buffer */
#define PA_SCC_WREQ     0x80
#define PB_RTC_DATA     0x01
#define PB_RTC_CLOCK    0x02
#define PB_RTC_ENABLE   0x04        /* active low */
#define PB_MOUSE_BTN    0x08        /* Plus: 0 = pressed; ADB: interrupt, active low */
#define PB_MOUSE_X2     0x10        /* Plus; ADB: ST0 */
#define PB_MOUSE_Y2     0x20        /* Plus; ADB: ST1 */
#define PB_HBLANK       0x40        /* Plus: 1 while the beam draws */
#define PB_SOUND_OFF    0x80

/* Called by the VIA when the Mac changes them */
void via_port_a_changed(uint8_t old_value, uint8_t value);
void via_port_b_changed(uint8_t old_value, uint8_t value);
uint8_t via_port_b_inputs(void);
void via_sr_written(void);          /* Mac wrote the shift register */
void via_acr_changed(uint8_t old_acr, uint8_t acr);

/* rtc.c */
void rtc_reset(void);
void rtc_lines(uint8_t port_b, uint8_t ddr_b);
bool rtc_data_out(void);

/* keyboard.c: Plus keyboard; adb.c: ADB keyboard and mouse */
void kbd_reset(void);
void kbd_sr_written(void);
void kbd_acr_changed(uint8_t old_acr, uint8_t acr);
void kbd_event(void);
void kbd_frame(void);
void adb_reset(void);
void adb_state_changed(uint8_t st);
void adb_event(void);
void adb_frame(void);
bool adb_interrupt_line(void);      /* PB3 level */
void adb_sr_written(void);
bool adb_mouse_active(void);        /* the Mac polls the ADB mouse */

/* input.c: events from other tasks */
enum { INPUT_KEY, INPUT_MOUSE, INPUT_MOUSE_ABS };
typedef struct {
    uint8_t type;
    uint8_t code;                   /* key code */
    uint8_t value;                  /* key down / mouse button */
    int16_t dx, dy;                 /* motion, or position */
} input_event_t;
bool input_next_key(uint8_t *code, bool *down);
bool input_key_waiting(void);
void input_reset(void);
void mouse_frame(void);             /* once a frame: take input, move the pointer */
bool mouse_button(void);
int adb_mouse_delta(int *dx, int *dy);      /* motion owed to the ADB mouse */
void adb_mouse_took(int dx, int dy);        /* ... and the part it reported */

/* scc.c */
void scc_reset(void);
uint8_t scc_read(uint32_t address);
void scc_write(uint32_t address, uint8_t value);
bool scc_interrupts_enabled(void);

/* iwm.c */
void iwm_reset(void);
uint8_t iwm_access(uint32_t address, bool write, uint8_t value);

/* scsi.c */
void scsi_reset(void);
uint8_t scsi_read(uint32_t address);
void scsi_write(uint32_t address, uint8_t value);

/* sony.c: the disk driver */
void sony_reset(void);
bool sony_install(uint8_t *rom, uint32_t rom_size, uint32_t offset);
int sony_native(uint16_t function);
void sony_frame(void);

/* rom.c */
bool rom_prepare(const uint8_t *image, uint32_t size);

/* video.c: screen and sound buffers */
uint32_t video_base(void);
void sound_frame_end(void);
void sound_reset(void);
