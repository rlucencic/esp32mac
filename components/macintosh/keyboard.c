/*
 * Mac Plus keyboard (M0110A) on the VIA shift register. The keyboard only
 * answers commands:
 *
 *   $10 Inquiry   the next key transition, or $7B (null) after ~1/4 s
 *   $14 Instant   the same, without waiting
 *   $16 Model     $0B
 *   $36 Test      $7D (ACK)
 *
 * The Mac writes the command into SR with shift-out under the external
 * clock (mode 7); the keyboard clocks it out a little later (SR interrupt).
 * The Mac then switches SR to shift-in (mode 3) and the keyboard clocks its
 * answer in (SR interrupt again).
 *
 * A key transition is (key code << 1) | 1, plus $80 when released. Keypad
 * and arrow keys answer $79 first, and their transition comes with the
 * Instant command the Mac sends next (code - $40, encoded the same way).
 */
#include "mac_internal.h"

#define CLOCK_CYCLES    2600        /* ~1/3 ms: the keyboard clocks a byte */
#define INQUIRY_FRAMES  16          /* the Mac resets a keyboard silent for 32/60 s */
#define NULL_REPLY      0x7B

enum { IDLE, COMMAND, ANSWERED, REPLYING };

static struct {
    int state;
    bool have_reply;
    uint8_t reply;
    int inquiry_frames;             /* waiting for a key, frames left */
    uint8_t instant;                /* second half of a keypad transition */
} kbd;

void kbd_reset(void)
{
    kbd.state = IDLE;
    kbd.have_reply = false;
    kbd.inquiry_frames = 0;
    kbd.instant = NULL_REPLY;
}

/* Keys a Plus keyboard has: the main block, the keypad, the arrows */
static bool plus_key(uint8_t code)
{
    return code < 0x40 || (code >= 0x41 && code <= 0x5C) || (code >= 0x7B && code <= 0x7E);
}

static bool next_transition(uint8_t *out)
{
    uint8_t code;
    bool down;
    while (input_next_key(&code, &down)) {
        if (!plus_key(code)) {
            continue;
        }
        uint8_t up = down ? 0 : 0x80;
        if (code < 0x40) {
            *out = (code << 1) | 1 | up;
        } else {
            *out = 0x79;
            kbd.instant = (((code - 0x40) & 0x3F) << 1) | 1 | up;
        }
        return true;
    }
    return false;
}

static void answer(uint8_t value)
{
    kbd.reply = value;
    kbd.have_reply = true;
    kbd.inquiry_frames = 0;
    if (via_sr_shifting_in()) {
        kbd.state = REPLYING;
        mac_schedule(EV_KBD, mac_now() + CLOCK_CYCLES);
    }
}

static void command(uint8_t cmd)
{
    uint8_t key;
    switch (cmd) {
    case 0x10:
        if (next_transition(&key)) {
            answer(key);
        } else {
            kbd.inquiry_frames = INQUIRY_FRAMES;
        }
        break;
    case 0x14:
        answer(kbd.instant);
        kbd.instant = NULL_REPLY;
        break;
    case 0x16:
        answer(0x0B);
        break;
    case 0x36:
        answer(0x7D);
        break;
    default:
        answer(0);
        break;
    }
}

/* The Mac put a command into SR */
void kbd_sr_written(void)
{
    if (via_sr_shifting_out()) {
        kbd.state = COMMAND;
        kbd.have_reply = false;
        kbd.inquiry_frames = 0;
        mac_schedule(EV_KBD, mac_now() + CLOCK_CYCLES);
    }
}

/* The Mac turned SR around to receive the answer */
void kbd_acr_changed(uint8_t old_acr, uint8_t acr)
{
    if (kbd.state == ANSWERED && via_sr_shifting_in() && kbd.have_reply) {
        kbd.state = REPLYING;
        mac_schedule(EV_KBD, mac_now() + CLOCK_CYCLES);
    }
}

void kbd_event(void)
{
    if (kbd.state == COMMAND) {
        uint8_t cmd = via_sr_send();
        kbd.state = ANSWERED;
        command(cmd);
    } else if (kbd.state == REPLYING) {
        kbd.state = IDLE;
        kbd.have_reply = false;
        via_sr_receive(kbd.reply);
    }
}

/* Once a frame: an Inquiry waiting for a key */
void kbd_frame(void)
{
    if (kbd.state != ANSWERED || kbd.have_reply || kbd.inquiry_frames == 0) {
        return;
    }
    uint8_t key;
    if (next_transition(&key)) {
        answer(key);
    } else if (--kbd.inquiry_frames == 0) {
        answer(NULL_REPLY);
    }
}
