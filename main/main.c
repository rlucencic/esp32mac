/*
 * Macintosh Plus / SE / Classic emulator on the Makerfabs MaTouch
 * ESP32-S3 7".
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * Mac into it: memory, ROM, settings, keyboard map, touch, disks, status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "emu/console.h"
#include "emu/emu.h"
#include "mac.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static mac_config_t s_cfg;
static uint8_t s_pram[MAC_PRAM_SIZE];
static volatile bool s_pram_dirty;
static const char *s_rom_name;

static const char *const model_names[] = { "Mac Plus", "Mac SE", "Classic", "" };
static const char *const model_full[] = { "Macintosh Plus", "Macintosh SE", "Macintosh Classic" };
static const char *const rom_files[] = { "plus", "se", "classic" };

/* ---- Memory ----
 *
 * Mac RAM (up to 4 MB in one block) and the ROM copy, first thing at
 * power-on, before the screen, WiFi and Bluetooth fragment PSRAM. The rest
 * of the board needs ~1.7 MB of it at most (screen buffer 1.2 MB, WiFi,
 * Bluetooth, setup screen).
 */
#define PSRAM_FOR_THE_REST      (1792 * 1024 + ROM_MAX_SIZE)

static uint16_t s_ram_limit_kb = 1024;

static uint32_t ram_wanted(void)
{
    uint16_t kb = settings.ram_kb < s_ram_limit_kb ? settings.ram_kb : s_ram_limit_kb;
    return kb * 1024u;
}

static void reserve_memory(void)
{
    settings_load();
    if (!settings_load_pram(s_pram)) {
        mac_pram_defaults(s_pram, settings.model);
    }
    s_cfg.opcode_tables = heap_caps_malloc(MAC_OPCODE_TABLES_SIZE, MALLOC_CAP_SPIRAM);

    static const uint16_t sizes[] = { 4096, 2048, 1024 };
    size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    room = room > PSRAM_FOR_THE_REST ? room - PSRAM_FOR_THE_REST : 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (sizes[i] * 1024u <= room) {
            s_ram_limit_kb = sizes[i];
            break;
        }
    }
    uint32_t ram_size = ram_wanted();
    if (ram_size / 1024 != settings.ram_kb) {
        ESP_LOGW(TAG, "no room for %u KB of Mac RAM, using %u KB", (unsigned)settings.ram_kb,
                 (unsigned)(ram_size / 1024));
    }
    s_cfg.ram = heap_caps_malloc(ram_size, MALLOC_CAP_SPIRAM);
    s_cfg.ram_size = ram_size;
    s_cfg.rom_copy = heap_caps_malloc(ROM_MAX_SIZE, MALLOC_CAP_SPIRAM);
}

/* ---- Parameter RAM: saved from a core 0 task, not the emulation ---- */

static void pram_changed(void)
{
    s_pram_dirty = true;
}

static void every_second(void)
{
    if (s_pram_dirty) {
        s_pram_dirty = false;
        settings_save_pram(s_pram);
    }
}

static void disk_ejected(int drive)
{
    disks_ejected(drive);
}

/* ---- Starting ---- */

static bool start(void)
{
    uint32_t rom_size;
    if (s_cfg.rom_copy == NULL ||
        !rom_store_load(settings.model, s_cfg.rom_copy, &rom_size, &s_rom_name)) {
        static char message[160];
        snprintf(message, sizeof(message),
                 "No %s ROM: put it in roms/%s.rom and flash with idf.py flash, "
                 "or copy it into the \"roms\" folder on the SD card.",
                 model_full[settings.model], rom_files[settings.model]);
        emu_set_message(message);
        return false;
    }
    uint32_t ram_size = ram_wanted();
    if (s_cfg.ram == NULL || s_cfg.ram_size != ram_size) {
        free(s_cfg.ram);
        s_cfg.ram = heap_caps_malloc(ram_size, MALLOC_CAP_SPIRAM);
        if (s_cfg.ram == NULL) {
            /* PSRAM is in pieces by now: restart the board, which takes the
             * memory first and goes straight into the Mac (the setting is saved)
             */
            ESP_LOGW(TAG, "no %u KB block for the Mac RAM now: restarting the board",
                     (unsigned)(ram_size / 1024));
            emu_reboot_into_machine();
        }
        s_cfg.ram_size = ram_size;
    }
    s_cfg.rom = s_cfg.rom_copy;     /* patched in place */
    s_cfg.rom_size = rom_size;
    s_cfg.pram = s_pram;
    s_cfg.pram_changed = pram_changed;
    s_cfg.disk_ejected = disk_ejected;
    if (!mac_init(&s_cfg)) {
        emu_set_message("The ROM could not be prepared: see the log.");
        return false;
    }
    return true;
}

static bool halted(void)
{
    mac_status_t s;
    mac_get_status(&s);
    return s.halted;
}

static void screen_mode(emu_screen_t *mode)
{
    mode->width = MAC_SCREEN_WIDTH;
    mode->height = MAC_SCREEN_HEIGHT;
    mode->hz = MAC_SCREEN_HZ;
}

/* ---- Keyboard, mouse, touch ---- */

/* HID keyboard usages 0x00-0x67 to Mac key codes (US positions); K() marks
 * an entry (Mac key code 0 is A), 0 = no Mac key
 */
#define K(code)     ((code) | 0x80)
static const uint8_t usage_to_mac[0x68] = {
    [0x04] = K(0x00), [0x05] = K(0x0B), [0x06] = K(0x08), [0x07] = K(0x02),     /* a b c d */
    [0x08] = K(0x0E), [0x09] = K(0x03), [0x0A] = K(0x05), [0x0B] = K(0x04),     /* e f g h */
    [0x0C] = K(0x22), [0x0D] = K(0x26), [0x0E] = K(0x28), [0x0F] = K(0x25),     /* i j k l */
    [0x10] = K(0x2E), [0x11] = K(0x2D), [0x12] = K(0x1F), [0x13] = K(0x23),     /* m n o p */
    [0x14] = K(0x0C), [0x15] = K(0x0F), [0x16] = K(0x01), [0x17] = K(0x11),     /* q r s t */
    [0x18] = K(0x20), [0x19] = K(0x09), [0x1A] = K(0x0D), [0x1B] = K(0x07),     /* u v w x */
    [0x1C] = K(0x10), [0x1D] = K(0x06),                                   /* y z */
    [0x1E] = K(0x12), [0x1F] = K(0x13), [0x20] = K(0x14), [0x21] = K(0x15),     /* 1 2 3 4 */
    [0x22] = K(0x17), [0x23] = K(0x16), [0x24] = K(0x1A), [0x25] = K(0x1C),     /* 5 6 7 8 */
    [0x26] = K(0x19), [0x27] = K(0x1D),                                   /* 9 0 */
    [0x28] = K(0x24),      /* Return */
    [0x29] = K(0x35),      /* Esc */
    [0x2A] = K(0x33),      /* Backspace -> Delete */
    [0x2B] = K(0x30),      /* Tab */
    [0x2C] = K(0x31),      /* Space */
    [0x2D] = K(0x1B),      /* - */
    [0x2E] = K(0x18),      /* = */
    [0x2F] = K(0x21),      /* [ */
    [0x30] = K(0x1E),      /* ] */
    [0x31] = K(0x2A),      /* \ */
    [0x32] = K(0x2A),      /* non-US # */
    [0x33] = K(0x29),      /* ; */
    [0x34] = K(0x27),      /* ' */
    [0x35] = K(0x32),      /* ` */
    [0x36] = K(0x2B),      /* , */
    [0x37] = K(0x2F),      /* . */
    [0x38] = K(0x2C),      /* / */
    [0x39] = K(0x39),      /* Caps Lock */
    [0x3A] = K(0x7A), [0x3B] = K(0x78), [0x3C] = K(0x63), [0x3D] = K(0x76),     /* F1-F4 */
    [0x3E] = K(0x60), [0x3F] = K(0x61), [0x40] = K(0x62), [0x41] = K(0x64),     /* F5-F8 */
    [0x42] = K(0x65), [0x43] = K(0x6D), [0x44] = K(0x67), [0x45] = K(0x6F),     /* F9-F12 */
    [0x46] = K(0x69), [0x47] = K(0x6B), [0x48] = K(0x71),                    /* F13-F15 */
    [0x49] = K(0x72),      /* Insert -> Help */
    [0x4A] = K(0x73),      /* Home */
    [0x4B] = K(0x74),      /* Page Up */
    [0x4C] = K(0x75),      /* Delete -> forward delete */
    [0x4D] = K(0x77),      /* End */
    [0x4E] = K(0x79),      /* Page Down */
    [0x4F] = K(0x7C),      /* Right */
    [0x50] = K(0x7B),      /* Left */
    [0x51] = K(0x7D),      /* Down */
    [0x52] = K(0x7E),      /* Up */
    [0x53] = K(0x47),      /* Num Lock -> Clear */
    [0x54] = K(0x4B),      /* keypad / */
    [0x55] = K(0x43),      /* keypad * */
    [0x56] = K(0x4E),      /* keypad - */
    [0x57] = K(0x45),      /* keypad + */
    [0x58] = K(0x4C),      /* keypad Enter */
    [0x59] = K(0x53), [0x5A] = K(0x54), [0x5B] = K(0x55),                    /* keypad 1 2 3 */
    [0x5C] = K(0x56), [0x5D] = K(0x57), [0x5E] = K(0x58),                    /* keypad 4 5 6 */
    [0x5F] = K(0x59), [0x60] = K(0x5B), [0x61] = K(0x5C),                    /* keypad 7 8 9 */
    [0x62] = K(0x52),      /* keypad 0 */
    [0x63] = K(0x41),      /* keypad . */
    [0x64] = K(0x0A),      /* non-US \ (ISO key left of Z) */
    [0x67] = K(0x51),      /* keypad = */
};

/* Modifier usages $E0-$E7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI ->
 * Control, Shift, Option, Command (one key each on the Mac)
 */
static const uint8_t modifier_to_mac[8] = { 0x3B, 0x38, 0x3A, 0x37, 0x3B, 0x38, 0x3A, 0x37 };

static uint8_t s_modifiers;

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        int bit = usage - 0xE0;
        uint8_t code = modifier_to_mac[bit];
        uint8_t mask = 0;
        for (int i = 0; i < 8; i++) {
            mask |= modifier_to_mac[i] == code ? 1u << i : 0;
        }
        bool was = (s_modifiers & mask) != 0;
        s_modifiers = pressed ? s_modifiers | (1u << bit) : s_modifiers & ~(1u << bit);
        bool now = (s_modifiers & mask) != 0;
        if (was != now) {
            mac_key(code, now);
        }
        return;
    }
    if (usage < sizeof(usage_to_mac) && usage_to_mac[usage]) {
        mac_key(usage_to_mac[usage] & 0x7F, pressed);
    }
}

static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    /* The Mac has one button: any of them */
    mac_mouse(dx, dy, buttons != 0);
}

/* A finger on the picture is the pointer: down there clicks */
static void touch(int x, int y, bool pressed)
{
    mac_mouse_position(x, y, pressed);
}

/* ---- Setup screen choices ---- */

static const char *const ram_names[] = { "1 MB", "2 MB", "4 MB", "" };
static const uint16_t ram_sizes[] = { 1024, 2048, 4096 };

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i >= MAC_MODEL_PLUS && i <= MAC_MODEL_CLASSIC) {
        settings.model = i;
        settings_save();
    }
}

static int ram_get(void)
{
    return settings.ram_kb >= 4096 ? 2 : settings.ram_kb >= 2048 ? 1 : 0;
}

static void ram_set(int i)
{
    if (i >= 0 && i < 3) {
        settings.ram_kb = ram_sizes[i];
        settings_save();
    }
}

static const emu_choice_t choices[] = {
    { "Model", model_names, model_get, model_set },
    { "Memory", ram_names, ram_get, ram_set },
};

/* ---- Disks ---- */

static const char *const extensions[] = {
    ".dsk", ".img", ".image", ".hfv", ".hda", ".iso", ".toast", NULL,
};
static const char *const drive_names[] = { "Drive 1", "Drive 2", "Drive 3" };
static const disks_size_t sizes[] = {
    { 800 * 1024, "800 KB floppy" },
    { 1440 * 1024, "1.4 MB floppy" },
    { 20 * 1024 * 1024, "20 MB hard disk" },
    { 100 * 1024 * 1024, "100 MB hard disk" },
    { 500 * 1024 * 1024, "500 MB hard disk" },
};

static const disks_config_t disks = {
    .folder = "disks",
    .extensions = extensions,
    .description = "Disk images (.DSK, .IMG, .IMAGE, .HFV, .ISO)",
    .title = "Disks",
    .button = "Disks",
    .nvs_namespace = "disks",
    .drives = MAC_DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".dsk",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 2000ull * 1024 * 1024,
    .insert = mac_disk_insert,
    .eject = mac_disk_eject,
    .path = mac_disk_path,
    .write_protected = mac_disk_write_protected,
    .create = mac_disk_create,
};

/* ---- Status ---- */

static void size_text(char *buf, size_t len, uint64_t bytes)
{
    if (bytes >= 1024 * 1024 && bytes % (1024 * 1024) == 0) {
        snprintf(buf, len, "%llu MB", (unsigned long long)(bytes >> 20));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, len, "%.1f MB", bytes / 1048576.0);
    } else {
        snprintf(buf, len, "%llu KB", (unsigned long long)(bytes >> 10));
    }
}

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_ROWS) {
        snprintf(text, sizeof(text), "%s, %lu MB", model_full[mac_model()],
                 (unsigned long)(s_cfg.ram_size >> 20));
        emu_json_row(j, "System", text);
        emu_json_row(j, "ROM", s_rom_name ? s_rom_name : "-");
        snprintf(text, sizeof(text), "%d × %d, black and white, %d Hz", MAC_SCREEN_WIDTH,
                 MAC_SCREEN_HEIGHT, MAC_SCREEN_HZ);
        emu_json_row(j, "Screen", text);
        uint32_t ram_free, system_free;
        if (mac_ram_free(&ram_free, &system_free)) {
            snprintf(text, sizeof(text), "%lu KB of %lu MB <span class=\"dim\">(system heap %lu KB free)</span>",
                     (unsigned long)(ram_free / 1024), (unsigned long)(s_cfg.ram_size >> 20),
                     (unsigned long)(system_free / 1024));
            emu_json_row_bar(j, "Free RAM", text, 100 - (int)((uint64_t)ram_free * 100 / s_cfg.ram_size));
        }
        return;
    }
    mac_disk_status_t ds;
    mac_disk_status(&ds);
    for (int d = 0; d < MAC_DRIVES; d++) {
        const char *name = disks_name(d);
        size_text(text, sizeof(text), mac_disk_size(d));
        emu_json_drive(j, drive_names[d], name, mac_disk_write_protected(d), ds.light == d,
                       ds.reads[d], ds.writes[d], name ? text : NULL);
    }
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    mac_status_t s;
    mac_get_status(&s);
    snprintf(buf, len, "frames %lu PC $%06lx SR $%04x Ticks %lu%s", (unsigned long)s.frames,
             (unsigned long)s.pc, s.sr, (unsigned long)s.ticks, s.halted ? " HALTED" : "");
}

/*   k <hex>            tap Mac key code <hex> (e.g. 24 Return)
 *   K <hex> <0|1>      Mac key code down (1) or up (0)
 *   m <dx> <dy> <btn>  move the mouse, button 0/1
 *   p <x> <y> <btn>    pointer at screen position x, y
 *   R <model> <ram KB> restart as model 0 Plus, 1 SE, 2 Classic with that RAM
 *   n                  the programmer's interrupt switch
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned code = strtoul(line + 1, NULL, 16);
        mac_key(code, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        mac_key(code, false);
        return true;
    }
    case 'K': {
        unsigned code = 0, down = 0;
        if (sscanf(line + 1, "%x %u", &code, &down) == 2) {
            mac_key(code, down);
        }
        return true;
    }
    case 'm': {
        int dx = 0, dy = 0, button = 0;
        sscanf(line + 1, "%d %d %d", &dx, &dy, &button);
        mac_mouse(dx, dy, button);
        return true;
    }
    case 'p': {
        int x = 0, y = 0, button = 0;
        sscanf(line + 1, "%d %d %d", &x, &y, &button);
        mac_mouse_position(x, y, button);
        return true;
    }
    case 'R': {
        int model = 0, ram_kb = 0;
        if (sscanf(line + 1, "%d %d", &model, &ram_kb) == 2 && model >= 0 && model <= 2 &&
            (ram_kb == 1024 || ram_kb == 2048 || ram_kb == 4096)) {
            settings.model = model;
            settings.ram_kb = ram_kb;
            settings_save();
            emu_restart();
        }
        return true;
    }
    case 'n':
        mac_interrupt_switch();
        return true;
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "Macintosh",
    .short_name = "Mac",
    .view_width = 898,          /* 512 x 342 at 600 / 342, square pixels */
    .scale = EMU_SCALE_SMOOTH,
    .reserve_memory = reserve_memory,
    .start = start,
    .reset = mac_reset,
    .run_frame = mac_run_frame,
    .halted = halted,
    .screen_mode = screen_mode,
    .screen_signature = mac_screen_signature,
    .render_line = mac_render_line_rgb565,
    .audio_hz = MAC_AUDIO_HZ,
    .audio_enable = mac_audio_enable,
    .audio_render = mac_audio_render,
    .audio_pending = mac_audio_pending,
    .hid_key = hid_key,
    .hid_mouse = hid_mouse,
    .touch = touch,
    .choices = choices,
    .choice_count = 2,
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
