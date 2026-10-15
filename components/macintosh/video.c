/*
 * The screen (512 x 342, 1 = black) and the sound buffer, both read
 * straight from RAM where the video and sound hardware would fetch them.
 *
 * Sound: the hardware takes the high byte of one word per scanline from the
 * sound buffer (370 per frame, 22.25 kHz), 8-bit unsigned, scaled by the
 * volume on PA0-PA2 and muted by PB7. The emulation fills a ring at the end
 * of each frame; mac_audio_render() drains it from the audio task.
 */
#include <stdlib.h>
#include <string.h>
#include "mac_internal.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

uint32_t video_base(void)
{
    return mac.ram_size - ((via_port_a() & PA_SCREEN_PAGE) ? SCREEN_OFFSET : SCREEN_OFFSET_ALT);
}

void mac_render_line_rgb565(int y, uint16_t *dst)
{
    if (y < 0 || y >= MAC_SCREEN_HEIGHT || mac.ram == NULL) {
        memset(dst, 0, MAC_SCREEN_WIDTH * sizeof(uint16_t));
        return;
    }
    const uint16_t *src = (const uint16_t *)(mac.ram + video_base() + y * (MAC_SCREEN_WIDTH / 8));
    for (int i = 0; i < MAC_SCREEN_WIDTH / 16; i++) {
        uint16_t w = src[i];
        for (int b = 15; b >= 0; b--) {
            *dst++ = (w >> b) & 1 ? 0x0000 : 0xFFFF;
        }
    }
}

uint32_t mac_screen_signature(void)
{
    if (mac.ram == NULL) {
        return 0;
    }
    uint32_t base = video_base();
    const uint32_t *p = (const uint32_t *)(mac.ram + base);
    uint32_t h = 2166136261u ^ base;
    for (int i = 0; i < SCREEN_BYTES / 4; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h ? h : 1;
}

/* ---- Sound ---- */

#define RING        4096            /* stereo frames, power of two */

static bool s_enabled;
static int16_t *s_ring;             /* RING stereo frames, made when sound is on */
static volatile uint32_t s_head, s_tail;

void mac_audio_enable(bool enable)
{
    if (enable && s_ring == NULL) {
        size_t bytes = RING * 2 * sizeof(int16_t);
#ifdef ESP_PLATFORM
        s_ring = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
        s_ring = malloc(bytes);
#endif
    }
    s_enabled = enable && s_ring != NULL;
}

void sound_reset(void)
{
}

void sound_frame_end(void)
{
    if (!s_enabled) {
        return;
    }
    uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    if (s_head - tail > RING - FRAME_LINES) {
        return;                     /* nobody is listening: drop the frame */
    }
    uint8_t pa = via_port_a();
    bool on = !(via_port_b() & PB_SOUND_OFF);
    int volume = pa & PA_SOUND_VOL;
    uint32_t base = mac.ram_size - ((pa & PA_SOUND_PAGE) ? SOUND_OFFSET : SOUND_OFFSET_ALT);
    const uint16_t *buf = (const uint16_t *)(mac.ram + base);
    for (int line = 0; line < FRAME_LINES; line++) {
        int16_t s = 0;
        if (on) {
            s = (int16_t)(((buf[line] >> 8) - 128) * 256 * volume / 7);
        }
        uint32_t i = (s_head + line) % RING;
        s_ring[i * 2] = s_ring[i * 2 + 1] = s;
    }
    __atomic_store_n(&s_head, s_head + FRAME_LINES, __ATOMIC_RELEASE);
}

int mac_audio_render(int16_t *dst, int frames)
{
    if (s_ring == NULL) {
        return 0;
    }
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    uint32_t avail = head - s_tail;
    int n = frames < (int)avail ? frames : (int)avail;
    for (int i = 0; i < n; i++) {
        uint32_t j = (s_tail + i) % RING;
        dst[i * 2] = s_ring[j * 2];
        dst[i * 2 + 1] = s_ring[j * 2 + 1];
    }
    __atomic_store_n(&s_tail, s_tail + n, __ATOMIC_RELEASE);
    return n;
}

int mac_audio_pending(void)
{
    return (int)(__atomic_load_n(&s_head, __ATOMIC_ACQUIRE) - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE));
}
