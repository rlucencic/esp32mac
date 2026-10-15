/*
 * Desktop build of the Macintosh core, for testing without the board:
 *
 *   mac_host <rom> [frames] [out.ppm] [ram_kb]
 *
 * Environment:
 *   MAC_DISK1 .. MAC_DISK3   disk images in drives 1 .. 3 at power-on
 *   MAC_PRAM=<file>          parameter RAM, loaded and saved (256 bytes)
 *   MAC_SCRIPT=<file>        lines "<frame> <command>":
 *                              key <hex> 0|1        Mac key code down / up
 *                              type <text>          type ASCII letters/digits/space
 *                              mouse <dx> <dy> <b>  relative motion, button
 *                              pos <x> <y> <b>      pointer at x, y
 *                              shot <file.ppm>      screenshot
 *                              insert <d> <path>    disk into drive d (1..3)
 *                              eject <d>
 *                              peek <hex addr>      print the word there
 *                              free                 the Mac's free RAM
 *                              units                the unit table's used entries
 *   MAC_STATUS=<n>           print PC, Ticks every n frames
 *   MAC_TRACE=<pc>[:<n>]     disassemble n instructions from when PC is reached
 *   MAC_DIS=<addr>[:<n>]     disassemble n instructions at addr (after loading)
 *   MAC_FAULT_DIS            on bus/address errors: code before the fault,
 *                            registers, the drive queue (errors are always logged)
 *   MAC_WAV=<file>           record the sound (22255 Hz mono WAV)
 *   MAC_QUIET                no frame summary
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "m68k.h"
#include "mac.h"

static uint8_t s_pram[MAC_PRAM_SIZE];
static const char *s_pram_file;

static void save_pram(void)
{
    if (s_pram_file) {
        FILE *f = fopen(s_pram_file, "wb");
        if (f) {
            fwrite(s_pram, 1, sizeof(s_pram), f);
            fclose(f);
        }
    }
}

static void ejected(int drive)
{
    printf("host: the Mac ejected drive %d\n", drive + 1);
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", MAC_SCREEN_WIDTH, MAC_SCREEN_HEIGHT);
    uint16_t line[MAC_SCREEN_WIDTH];
    for (int y = 0; y < MAC_SCREEN_HEIGHT; y++) {
        mac_render_line_rgb565(y, line);
        for (int x = 0; x < MAC_SCREEN_WIDTH; x++) {
            uint8_t v = line[x] ? 255 : 0;
            uint8_t px[3] = { v, v, v };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    printf("host: screenshot %s\n", path);
}

/* ---- Tracing ---- */

static uint32_t s_trace_pc = 0xFFFFFFFF;
static int s_trace_left;
static bool s_tracing;

void m68k_host_instruction(unsigned int pc)
{
    if (!s_tracing && pc == s_trace_pc && s_trace_left > 0) {
        s_tracing = true;
    }
    if (s_tracing) {
        char buf[100];
        m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        printf("  %06x  %-40s D0=%08x A0=%08x A1=%08x A7=%08x\n", pc, buf,
               m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_A0),
               m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A7));
        if (--s_trace_left <= 0) {
            s_tracing = false;
        }
    }
}

static void disassemble(uint32_t addr, int n)
{
    for (int i = 0; i < n; i++) {
        char buf[100];
        int len = m68k_disassemble(buf, addr, M68K_CPU_TYPE_68000);
        printf("  %06x  %04x  %s\n", addr, mac_peek16(addr), buf);
        addr += len;
    }
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[512];
} line_t;

static line_t *s_script;
static int s_lines, s_next;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "no script %s\n", path);
        exit(1);
    }
    s_script = calloc(4096, sizeof(line_t));
    char buf[600];
    while (s_lines < 4096 && fgets(buf, sizeof(buf), f)) {
        line_t *l = &s_script[s_lines];
        int at = 0;
        if (buf[0] == '#' || sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &at) < 2) {
            continue;
        }
        snprintf(l->arg, sizeof(l->arg), "%s", buf + at);
        l->arg[strcspn(l->arg, "\r\n")] = 0;
        s_lines++;
    }
    fclose(f);
}

/* ASCII to Mac key codes (US layout), for "type" */
static int ascii_key(char c, bool *shift)
{
    static const char *lower = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\0 `";
    *shift = false;
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        c = c - 'A' + 'a';
    }
    for (int i = 0; lower[i] || i < 0x33; i++) {
        if (lower[i] == c && c) {
            return i;
        }
    }
    if (c == '\n') {
        return 0x24;
    }
    return -1;
}

static int s_type_delay;

static void type_text(const char *text)
{
    /* Queued as key events; the keyboard hands them out one at a time */
    for (const char *p = text; *p; p++) {
        bool shift;
        int k = ascii_key(*p, &shift);
        if (k < 0) {
            continue;
        }
        if (shift) {
            mac_key(0x38, true);
        }
        mac_key(k, true);
        mac_key(k, false);
        if (shift) {
            mac_key(0x38, false);
        }
    }
    (void)s_type_delay;
}

static void run_script(uint32_t frame)
{
    while (s_next < s_lines && s_script[s_next].frame <= frame) {
        line_t *l = &s_script[s_next++];
        if (strcmp(l->cmd, "key") == 0) {
            unsigned code = 0, down = 0;
            sscanf(l->arg, "%x %u", &code, &down);
            mac_key(code, down);
        } else if (strcmp(l->cmd, "type") == 0) {
            type_text(l->arg);
        } else if (strcmp(l->cmd, "mouse") == 0) {
            int dx = 0, dy = 0, b = 0;
            sscanf(l->arg, "%d %d %d", &dx, &dy, &b);
            mac_mouse(dx, dy, b);
        } else if (strcmp(l->cmd, "pos") == 0) {
            int x = 0, y = 0, b = 0;
            sscanf(l->arg, "%d %d %d", &x, &y, &b);
            mac_mouse_position(x, y, b);
        } else if (strcmp(l->cmd, "shot") == 0) {
            write_ppm(l->arg);
        } else if (strcmp(l->cmd, "insert") == 0) {
            int d = 0, at = 0;
            sscanf(l->arg, "%d %n", &d, &at);
            if (!mac_disk_insert(d - 1, l->arg + at)) {
                printf("host: can't insert %s\n", l->arg + at);
            }
        } else if (strcmp(l->cmd, "peek") == 0) {
            unsigned a = 0;
            sscanf(l->arg, "%x", &a);
            printf("host: frame %u: $%06x = $%04x\n", frame, a, mac_peek16(a));
        } else if (strcmp(l->cmd, "free") == 0) {
            uint32_t f, sf;
            if (mac_ram_free(&f, &sf)) {
                printf("host: frame %u: %u KB free, system heap %u KB free\n", frame, f / 1024, sf / 1024);
            } else {
                printf("host: frame %u: free memory not known yet\n", frame);
            }
        } else if (strcmp(l->cmd, "units") == 0) {
            uint32_t base = (mac_peek16(0x11C) << 16 | mac_peek16(0x11E)) & 0xFFFFFF;
            int count = (int16_t)mac_peek16(0x1D2);
            printf("host: unit table at $%06x, %d entries; used:", base, count);
            for (int u = 0; u < count; u++) {
                if (mac_peek16(base + 4 * u) || mac_peek16(base + 4 * u + 2)) {
                    printf(" %d", u);
                }
            }
            printf("\n");
        } else if (strcmp(l->cmd, "eject") == 0) {
            mac_disk_eject(atoi(l->arg) - 1);
        } else {
            printf("host: unknown script command %s\n", l->cmd);
        }
    }
}

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom> [frames] [out.ppm] [ram_kb]\n", argv[0]);
        return 1;
    }
    uint32_t rom_size;
    uint8_t *rom = read_file(argv[1], &rom_size);
    if (rom == NULL) {
        fprintf(stderr, "can't read %s\n", argv[1]);
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    const char *out = argc > 3 ? argv[3] : NULL;
    uint32_t ram_kb = argc > 4 ? atoi(argv[4]) : 4096;

    s_pram_file = getenv("MAC_PRAM");
    if (s_pram_file) {
        FILE *f = fopen(s_pram_file, "rb");
        if (f) {
            fread(s_pram, 1, sizeof(s_pram), f);
            fclose(f);
        }
    }

    mac_config_t cfg = {
        .ram = malloc(ram_kb * 1024),
        .ram_size = ram_kb * 1024,
        .rom = rom,
        .rom_size = rom_size,
        .rom_copy = malloc(rom_size),
        .pram = s_pram,
        .pram_changed = save_pram,
        .disk_ejected = ejected,
    };
    if (!mac_init(&cfg)) {
        fprintf(stderr, "mac_init failed\n");
        return 1;
    }
    for (int d = 0; d < MAC_DRIVES; d++) {
        char name[16];
        snprintf(name, sizeof(name), "MAC_DISK%d", d + 1);
        const char *path = getenv(name);
        if (path && !mac_disk_insert(d, path)) {
            fprintf(stderr, "can't insert %s\n", path);
        }
    }
    const char *script = getenv("MAC_SCRIPT");
    if (script) {
        load_script(script);
    }
    const char *trace = getenv("MAC_TRACE");
    if (trace) {
        s_trace_left = 200;
        sscanf(trace, "%x:%d", &s_trace_pc, &s_trace_left);
    }
    const char *dis = getenv("MAC_DIS");
    if (dis) {
        unsigned addr = 0;
        int n = 40;
        sscanf(dis, "%x:%d", &addr, &n);
        disassemble(addr, n);
        return 0;
    }
    int status_every = getenv("MAC_STATUS") ? atoi(getenv("MAC_STATUS")) : 0;
    FILE *wav = getenv("MAC_WAV") ? fopen(getenv("MAC_WAV"), "wb") : NULL;
    uint32_t wav_frames = 0;
    if (wav) {
        mac_audio_enable(true);
        fseek(wav, 44, SEEK_SET);
    }

    clock_t start = clock();
    for (int f = 0; f < frames; f++) {
        run_script(f);
        mac_run_frame();
        if (wav) {
            int16_t buf[1024 * 2];
            int n;
            while ((n = mac_audio_render(buf, 1024)) > 0) {
                for (int i = 0; i < n; i++) {
                    fwrite(&buf[i * 2], 2, 1, wav);
                }
                wav_frames += n;
            }
        }
        if (status_every && (f + 1) % status_every == 0) {
            mac_status_t s;
            mac_get_status(&s);
            printf("frame %d: PC $%06x SR $%04x Ticks %u%s\n", f + 1, s.pc, s.sr, s.ticks,
                   s.halted ? " HALTED" : "");
        }
    }
    double secs = (double)(clock() - start) / CLOCKS_PER_SEC;
    if (!getenv("MAC_QUIET")) {
        mac_status_t s;
        mac_get_status(&s);
        printf("%d frames in %.2f s (%.0f%% of real time), PC $%06x, Ticks %u%s\n", frames, secs,
               frames / 60.15 / secs * 100, s.pc, s.ticks, s.halted ? ", HALTED" : "");
        mac_disk_status_t ds;
        mac_disk_status(&ds);
        for (int d = 0; d < MAC_DRIVES; d++) {
            if (mac_disk_path(d)) {
                printf("drive %d: %u blocks read, %u written\n", d + 1, ds.reads[d], ds.writes[d]);
            }
        }
    }
    if (out) {
        write_ppm(out);
    }
    if (wav) {
        /* RIFF header: mono, 16-bit */
        uint32_t data = wav_frames * 2, rate = MAC_AUDIO_HZ, bytes = rate * 2;
        uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x01\0........\x02\0\x10\0data....";
        uint32_t riff = 36 + data;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &bytes, 4);
        memcpy(h + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
        printf("host: %u sound frames\n", wav_frames);
    }
    return 0;
}
