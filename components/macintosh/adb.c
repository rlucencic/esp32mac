/*
 * ADB (SE, Classic): the transceiver behind the VIA shift register, with
 * a keyboard at address 2 and a mouse at address 3.
 *
 * The Mac drives the transceiver state on PB4/PB5 (ST0/ST1):
 *   0  start a command: the command byte is in SR
 *   1  even data byte, 2 odd data byte (talk: device -> SR; listen: SR -> device)
 *   3  idle: the transceiver polls the last device by itself
 * and sees PB3 (/ADB interrupt) go low when a talk had no data (time-out).
 * A command byte is aaaa cc rr: address, command (0 reset/flush, 2 listen,
 * 3 talk), register.
 *
 * The state sequence follows Mini vMac's, which the SE and Classic ROMs
 * are known to be happy with.
 */
#include <string.h>
#include "mac_internal.h"

#define STATE_CYCLES    5440        /* ~0.7 ms after a state change */
#define MAX_DATA        8

static struct {
    uint8_t state;
    uint8_t command;
    bool listening;
    bool talk_data;
    uint8_t data[MAX_DATA];
    uint8_t size, index;
    bool int_line;                  /* PB3: high = nothing to report */
    uint8_t kbd_address, mouse_address;
    uint8_t random;
    bool mouse_button;              /* as last reported */
    bool mouse_active;              /* the Mac polls the mouse */
} adb;

void adb_reset(void)
{
    uint8_t random = adb.random;
    memset(&adb, 0, sizeof(adb));
    adb.random = random + 1;
    adb.int_line = true;
    adb.kbd_address = 2;
    adb.mouse_address = 3;
}

bool adb_interrupt_line(void)
{
    return adb.int_line;
}

bool adb_mouse_active(void)
{
    return adb.mouse_active;
}

void adb_sr_written(void)
{
}

void adb_state_changed(uint8_t st)
{
    adb.state = st;
    mac_schedule(EV_ADB, mac_now() + STATE_CYCLES);
}

/* ADB key codes differ from the Mac's virtual ones for a few keys */
static uint8_t adb_key(uint8_t code)
{
    switch (code) {
    case 0x3B: return 0x36;     /* Control */
    case 0x7B: return 0x3B;     /* arrows: left, right, down, up */
    case 0x7C: return 0x3C;
    case 0x7D: return 0x3D;
    case 0x7E: return 0x3E;
    default:   return code;
    }
}

static void talk(uint8_t a, uint8_t b)
{
    adb.data[0] = a;
    adb.data[1] = b;
    adb.size = 2;
    adb.talk_data = true;
}

static void register3(void)
{
    talk(0x60 | (adb.random++ & 0x0F), 0x01);
}

static int clamp63(int v)
{
    return v > 63 ? 63 : v < -63 ? -63 : v;
}

static void mouse_talk(void)
{
    switch (adb.command & 3) {
    case 0: {
        int dx, dy;
        adb_mouse_delta(&dx, &dy);
        dx = clamp63(dx);
        dy = clamp63(dy);
        bool button = mouse_button();
        adb.mouse_active = true;
        if (dx || dy || button != adb.mouse_button) {
            adb_mouse_took(dx, dy);
            adb.mouse_button = button;
            talk((button ? 0x00 : 0x80) | (dy & 0x7F), 0x80 | (dx & 0x7F));
        }
        break;
    }
    case 3:
        register3();
        break;
    }
}

static void keyboard_talk(void)
{
    switch (adb.command & 3) {
    case 0: {
        uint8_t code;
        bool down;
        if (input_next_key(&code, &down)) {
            uint8_t first = adb_key(code) | (down ? 0 : 0x80);
            uint8_t second = 0xFF;
            if (input_next_key(&code, &down)) {
                second = adb_key(code) | (down ? 0 : 0x80);
            }
            talk(first, second);
        }
        break;
    }
    case 3:
        register3();
        break;
    }
}

static void do_talk(void)
{
    uint8_t address = adb.command >> 4;
    if (address == adb.mouse_address) {
        mouse_talk();
    } else if (address == adb.kbd_address) {
        keyboard_talk();
    }
}

static void end_listen(void)
{
    uint8_t address = adb.command >> 4;
    if ((adb.command & 3) == 3 && adb.size >= 2 && adb.data[1] == 0xFE) {
        /* Change the device's address */
        if (address == adb.mouse_address) {
            adb.mouse_address = adb.data[0] & 0x0F;
        } else if (address == adb.kbd_address) {
            adb.kbd_address = adb.data[0] & 0x0F;
        }
    }
}

static bool anything_to_report(void)
{
    int dx, dy;
    return input_key_waiting() || adb_mouse_delta(&dx, &dy) || mouse_button() != adb.mouse_button;
}


#ifdef ADB_DEBUG
#define ADB_LOG(...)    printf(__VA_ARGS__)
#else
#define ADB_LOG(...)
#endif

void adb_event(void)
{
    ADB_LOG("adb f%u state %d cmd %02x talk %d idx %d/%d\n", (unsigned)mac.frames, adb.state,
            adb.command, adb.talk_data, adb.index, adb.size);
    adb.int_line = true;
    switch (adb.state) {
    case 0:
        if (adb.listening) {
            adb.listening = false;
            adb.size = adb.index;
            end_listen();
        }
        adb.talk_data = false;
        adb.index = 0;
        adb.command = via_sr_send();
        switch ((adb.command >> 2) & 3) {
        case 0:
            if ((adb.command & 3) == 0) {
                adb.kbd_address = 2;
                adb.mouse_address = 3;
            } else if ((adb.command & 3) == 1) {
                talk(0, 0);         /* flush */
            }
            break;
        case 2:
            adb.listening = true;
            break;
        case 3:
            do_talk();
            break;
        }
        break;

    case 1:
    case 2:
        if (adb.listening) {
            uint8_t b = via_sr_send();
            if (adb.index < MAX_DATA) {
                adb.data[adb.index++] = b;
            }
        } else if (!adb.talk_data || adb.index >= adb.size) {
            via_sr_receive(0xFF);
            adb.int_line = false;       /* time-out: no (more) data */
        } else {
            via_sr_receive(adb.data[adb.index++]);
        }
        break;

    case 3:
        if (adb.talk_data) {
            via_sr_receive(0xFF);
        } else if (anything_to_report()) {
            if (((adb.command >> 2) & 3) == 3) {
                do_talk();
            }
            via_sr_receive(0xFF);
        }
        break;
    }
}

/* Once a frame: idle with something new to report gets the Mac's attention */
void adb_frame(void)
{
    if (mac.adb && adb.state == 3 && !adb.talk_data && anything_to_report()) {
        if (((adb.command >> 2) & 3) == 3) {
            do_talk();
        }
        via_sr_receive(0xFF);
    }
}
