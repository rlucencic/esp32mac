/*
 * VIA 6522 at $EFE1FE (register n at + n * $200, even bytes).
 *
 * Timers count at CPU / 10 and are kept as (start time, start value), so
 * they cost nothing between their scheduled expiry events. The shift
 * register talks to the keyboard (Plus) or the ADB transceiver (SE,
 * Classic), both under the external clock: the device side is
 * via_sr_send() / via_sr_receive().
 */
#include <string.h>
#include "mac_internal.h"

enum {
    R_ORB, R_ORA, R_DDRB, R_DDRA, R_T1CL, R_T1CH, R_T1LL, R_T1LH,
    R_T2CL, R_T2CH, R_SR, R_ACR, R_PCR, R_IFR, R_IER, R_ORA_NH,
};

#define ACR_T1_FREE     0x40
#define ACR_SR_MODE(a)  (((a) >> 2) & 7)
#define SR_IN_EXT       3
#define SR_OUT_SYS      6
#define SR_OUT_EXT      7

static struct {
    uint8_t ora, orb, ddra, ddrb;
    uint8_t sr, acr, pcr, ifr, ier;
    uint8_t t1ll, t1lh, t2ll;
    uint64_t t1_start;              /* cycle time the counter held t1_value */
    uint16_t t1_value;
    bool t1_armed;                  /* one-shot: an interrupt still due */
    uint64_t t2_start;
    uint16_t t2_value;
    bool t2_armed;
} via;

/* Effective output levels: inputs (DDR 0) read as pulled high */
uint8_t via_port_a(void)
{
    return (via.ora & via.ddra) | ~via.ddra;
}

uint8_t via_port_b(void)
{
    return (via.orb & via.ddrb) | ~via.ddrb;
}

static void update_irq(void)
{
    bool irq = (via.ifr & via.ier & 0x7F) != 0;
    if (irq != mac.via_irq) {
        mac.via_irq = irq;
        mac_update_irq();
    }
}

void via_set_flag(int flag)
{
    via.ifr |= flag;
    update_irq();
}

static void clear_flag(int flag)
{
    via.ifr &= ~flag;
    update_irq();
}

/* ---- Timers ---- */

static uint16_t counter(uint64_t start, uint16_t value)
{
    uint64_t ticks = (mac_now() - start) / VIA_DIVIDER;
    return (uint16_t)(value - ticks);
}

static void schedule_t1(void)
{
    if (via.t1_armed || (via.acr & ACR_T1_FREE)) {
        mac_schedule(EV_VIA_T1, via.t1_start + ((uint64_t)via.t1_value + 1) * VIA_DIVIDER);
    } else {
        mac_schedule(EV_VIA_T1, EV_NEVER);
    }
}

static void schedule_t2(void)
{
    mac_schedule(EV_VIA_T2, via.t2_armed ?
                 via.t2_start + ((uint64_t)via.t2_value + 1) * VIA_DIVIDER : EV_NEVER);
}

void via_timer_event(int event)
{
    uint64_t when = mac.events[event];
    if (event == EV_VIA_T1) {
        if (via.acr & ACR_T1_FREE) {
            /* Reload from the latches, one more tick for the reload */
            via.t1_start = when + VIA_DIVIDER;
            via.t1_value = (via.t1lh << 8) | via.t1ll;
            via_set_flag(VIA_IF_T1);
        } else {
            if (via.t1_armed) {
                via.t1_armed = false;
                via_set_flag(VIA_IF_T1);
            }
        }
        schedule_t1();
    } else {
        if (via.t2_armed) {
            via.t2_armed = false;
            via_set_flag(VIA_IF_T2);
        }
        schedule_t2();
    }
}

/* ---- Shift register (device side) ---- */

bool via_sr_shifting_in(void)
{
    return ACR_SR_MODE(via.acr) == SR_IN_EXT;
}

bool via_sr_shifting_out(void)
{
    return ACR_SR_MODE(via.acr) == SR_OUT_EXT;
}

void via_sr_receive(uint8_t value)
{
    if (via_sr_shifting_in()) {
        via.sr = value;
        via_set_flag(VIA_IF_SR | VIA_IF_CB1);
    }
}

uint8_t via_sr_send(void)
{
    if (!via_sr_shifting_out()) {
        return 0;
    }
    via_set_flag(VIA_IF_SR | VIA_IF_CB1);
    return via.sr;
}

/* ---- Interrupt sources ---- */

void via_vbl(void)
{
    via_set_flag(VIA_IF_CA1);
}

void via_one_second(void)
{
    via_set_flag(VIA_IF_CA2);
}

/* ---- Registers ---- */

static void port_a_write(uint8_t ora, uint8_t ddra)
{
    uint8_t before = via_port_a();
    via.ora = ora;
    via.ddra = ddra;
    uint8_t after = via_port_a();
    if (before != after) {
        via_port_a_changed(before, after);
    }
}

static void port_b_write(uint8_t orb, uint8_t ddrb)
{
    uint8_t before = via_port_b();
    via.orb = orb;
    via.ddrb = ddrb;
    uint8_t after = via_port_b();
    if (before != after) {
        via_port_b_changed(before, after);
    }
    rtc_lines(via.orb, via.ddrb);
}

void via_reset(void)
{
    uint8_t a = via_port_a(), b = via_port_b();
    memset(&via, 0, sizeof(via));
    mac.via_irq = false;
    mac_update_irq();
    mac_schedule(EV_VIA_T1, EV_NEVER);
    mac_schedule(EV_VIA_T2, EV_NEVER);
    /* Everything is an input now (reads as high): overlay back on (Plus) */
    via_port_a_changed(a, via_port_a());
    via_port_b_changed(b, via_port_b());
    rtc_lines(via.orb, via.ddrb);
}

uint8_t via_read(uint32_t address)
{
    switch ((address >> 9) & 0xF) {
    case R_ORB:
        return (via.orb & via.ddrb) | (via_port_b_inputs() & ~via.ddrb);
    case R_ORA:
    case R_ORA_NH:
        if ((via.pcr & 0x0E) == 0) {
            clear_flag(VIA_IF_CA2);
        }
        clear_flag(VIA_IF_CA1);
        /* PA7 (SCC wait/request) reads high: nothing waiting */
        return (via.ora & via.ddra) | (0xFF & ~via.ddra);
    case R_DDRB:
        return via.ddrb;
    case R_DDRA:
        return via.ddra;
    case R_T1CL:
        clear_flag(VIA_IF_T1);
        return counter(via.t1_start, via.t1_value) & 0xFF;
    case R_T1CH:
        return counter(via.t1_start, via.t1_value) >> 8;
    case R_T1LL:
        return via.t1ll;
    case R_T1LH:
        return via.t1lh;
    case R_T2CL:
        clear_flag(VIA_IF_T2);
        return counter(via.t2_start, via.t2_value) & 0xFF;
    case R_T2CH:
        return counter(via.t2_start, via.t2_value) >> 8;
    case R_SR:
        clear_flag(VIA_IF_SR);
        return via.sr;
    case R_ACR:
        return via.acr;
    case R_PCR:
        return via.pcr;
    case R_IFR:
        return via.ifr | ((via.ifr & via.ier & 0x7F) ? 0x80 : 0);
    case R_IER:
        return via.ier | 0x80;
    }
    return 0;
}

void via_write(uint32_t address, uint8_t v)
{
    switch ((address >> 9) & 0xF) {
    case R_ORB:
        port_b_write(v, via.ddrb);
        break;
    case R_ORA:
    case R_ORA_NH:
        if ((via.pcr & 0x0E) == 0) {
            clear_flag(VIA_IF_CA2);
        }
        clear_flag(VIA_IF_CA1);
        port_a_write(v, via.ddra);
        break;
    case R_DDRB:
        port_b_write(via.orb, v);
        break;
    case R_DDRA:
        port_a_write(via.ora, v);
        break;
    case R_T1CL:
    case R_T1LL:
        via.t1ll = v;
        break;
    case R_T1CH:
        via.t1lh = v;
        via.t1_value = (v << 8) | via.t1ll;
        via.t1_start = mac_now();
        via.t1_armed = true;
        clear_flag(VIA_IF_T1);
        schedule_t1();
        break;
    case R_T1LH:
        via.t1lh = v;
        clear_flag(VIA_IF_T1);
        break;
    case R_T2CL:
        via.t2ll = v;
        break;
    case R_T2CH:
        via.t2_value = (v << 8) | via.t2ll;
        via.t2_start = mac_now();
        via.t2_armed = true;
        clear_flag(VIA_IF_T2);
        schedule_t2();
        break;
    case R_SR:
        via.sr = v;
        clear_flag(VIA_IF_SR);
        via_sr_written();
        break;
    case R_ACR: {
        uint8_t old = via.acr;
        if ((old ^ v) & ACR_T1_FREE) {
            /* Keep the count, change the mode */
            via.t1_value = counter(via.t1_start, via.t1_value);
            via.t1_start = mac_now();
        }
        via.acr = v;
        if (ACR_SR_MODE(v) == 0) {
            clear_flag(VIA_IF_SR);
        }
        schedule_t1();
        via_acr_changed(old, v);
        break;
    }
    case R_PCR:
        via.pcr = v;
        break;
    case R_IFR:
        via.ifr &= ~v & 0x7F;
        update_irq();
        break;
    case R_IER:
        if (v & 0x80) {
            via.ier |= v & 0x7F;
        } else {
            via.ier &= ~v & 0x7F;
        }
        update_irq();
        break;
    }
}
