/*
 * Macintosh machine: reset, the frame loop with its event scheduler,
 * interrupts and how the VIA's port lines are wired.
 *
 * Time is counted in CPU cycles. The CPU runs until the next scheduled
 * event (VIA timers, keyboard/ADB replies) or the next frame landmark
 * (vertical blanking at line 342, the end of the frame).
 */
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#include "mac_internal.h"

mac_machine_t mac;

static bool s_reset_pending;            /* RESET instruction on a Plus */

uint64_t mac_now(void)
{
    return mac.cpu_running ? mac.now + m68k_cycles_run() : mac.now;
}

void mac_schedule(int event, uint64_t when)
{
    mac.events[event] = when;
    /* Sooner than the CPU was going to stop: stop it there */
    if (mac.cpu_running && when < mac.now + mac.slice) {
        m68k_end_timeslice();
    }
}

void mac_update_irq(void)
{
    /* VIA on IPL0, SCC on IPL1, the interrupt switch on IPL2 */
    int level = (mac.via_irq ? 1 : 0) | (mac.scc_irq ? 2 : 0) | (mac.nmi ? 4 : 0);
    m68k_set_irq(level);
}

void mac_set_overlay(bool on)
{
    if (on != mac.overlay) {
        mac.overlay = on;
        mac.ram_window = on ? 0 : 0x400000;
    }
}

uint32_t mac_ram_top(void)
{
    return mac.ram_size;
}

mac_model_t mac_model(void)
{
    return mac.model;
}

/* ---- VIA port wiring ---- */

void via_port_a_changed(uint8_t old_value, uint8_t value)
{
    if (!mac.adb) {
        mac_set_overlay((value & PA_OVERLAY) != 0);
    }
}

void via_port_b_changed(uint8_t old_value, uint8_t value)
{
    if (mac.adb && ((old_value ^ value) & (PB_MOUSE_X2 | PB_MOUSE_Y2))) {
        adb_state_changed((value >> 4) & 3);
    }
}

/* The beam is drawing pixels (not in horizontal or vertical blanking) */
static bool beam_in_video(void)
{
    uint64_t t = mac_now() - mac.frame_start;
    return t < (uint64_t)MAC_SCREEN_HEIGHT * LINE_CYCLES && t % LINE_CYCLES < VISIBLE_CYCLES;
}

uint8_t via_port_b_inputs(void)
{
    uint8_t v = PB_SOUND_OFF;
    if (rtc_data_out()) {
        v |= PB_RTC_DATA;
    }
    if (mac.adb) {
        if (adb_interrupt_line()) {
            v |= PB_MOUSE_BTN;
        }
    } else {
        if (!mouse_button()) {
            v |= PB_MOUSE_BTN;
        }
        if (beam_in_video()) {
            v |= PB_HBLANK;
        }
    }
    return v;
}

void via_sr_written(void)
{
    if (mac.adb) {
        adb_sr_written();
    } else {
        kbd_sr_written();
    }
}

void via_acr_changed(uint8_t old_acr, uint8_t acr)
{
    if (!mac.adb) {
        kbd_acr_changed(old_acr, acr);
    }
}

/* ---- Musashi hooks ---- */

static int int_ack(int level)
{
    return M68K_INT_ACK_AUTOVECTOR;
}

/* RESET resets the peripherals. On a Plus the Finder's Restart does it
 * and expects the whole machine to restart.
 */
static void reset_instruction(void)
{
    if (!mac.adb) {
        s_reset_pending = true;
        m68k_end_timeslice();
        return;
    }
    via_reset();
    scc_reset();
    iwm_reset();
    scsi_reset();
}

/* Native calls from the replacement disk driver: ILLEGAL ($4AFC) followed
 * by a function word, only inside the driver in the ROM
 */
#define NATIVE_OPCODE   0x4AFC

int m68k_host_illegal(int opcode)
{
    if (opcode != NATIVE_OPCODE) {
        return 0;
    }
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
    uint32_t at = pc - 2;
    bool in_rom = (at & 0xF00000) == 0x400000 || (mac.overlay && at < 0x400000);
    uint32_t offset = at & mac.rom_mask;
    if (!in_rom || offset < mac.driver_start || offset >= mac.driver_end) {
        return 0;
    }
    uint16_t function = mac_peek16(pc);
    m68k_set_reg(M68K_REG_PC, pc + 2);
    int result = sony_native(function);
    m68k_set_reg(M68K_REG_D0, (uint32_t)result);
    return 1;
}

void m68k_host_halted(void)
{
    if (!mac.halted) {
        mac.halted = true;
        MAC_LOGW("CPU halted (double bus/address fault) at PC $%06lx",
                 (unsigned long)(m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF));
    }
}

void m68k_host_exception(unsigned int vector)
{
#ifndef ESP_PLATFORM
    /* Host build: bus and address errors, with the 68000's group 0 frame
     * (function code, access address, instruction, SR, PC)
     */
    static int logged;
    if ((vector == 2 || vector == 3) && logged < 8) {
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        logged++;
        MAC_LOGW("%s error: address $%06lx, PC $%06lx, opcode $%04x", vector == 2 ? "bus" : "address",
                 (unsigned long)(mac_peek32(sp + 2) & 0xFFFFFF), (unsigned long)(mac_peek32(sp + 10) & 0xFFFFFF),
                 mac_peek16(sp + 6));
        if (getenv("MAC_FAULT_DIS")) {
            uint32_t pc = mac_peek32(sp + 10) & 0xFFFFFF;
            for (uint32_t a = pc - 0x40; a <= pc + 4;) {
                char buf[100];
                int len = m68k_disassemble(buf, a, M68K_CPU_TYPE_68000);
                printf("    %06lx  %s\n", (unsigned long)a, buf);
                a += len;
            }
            uint32_t e = mac_peek32(0x30A) & 0xFFFFFF;
            for (int n = 0; e && n < 10; n++) {
                printf("    drive queue: $%06lx drive %d refnum %d qType %d size %u\n", (unsigned long)e,
                       (int16_t)mac_peek16(e + 6), (int16_t)mac_peek16(e + 8), mac_peek16(e + 4),
                       mac_peek16(e + 12) | (mac_peek16(e + 14) << 16));
                e = mac_peek32(e) & 0xFFFFFF;
            }
            printf("    SonyVars $%06lx\n", (unsigned long)(mac_peek32(0x134) & 0xFFFFFF));
            printf("    D0=%08x D1=%08x A0=%08x A1=%08x A2=%08x A3=%08x A4=%08x A5=%08x A6=%08x\n",
                   m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                   m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                   m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A3),
                   m68k_get_reg(NULL, M68K_REG_A4), m68k_get_reg(NULL, M68K_REG_A5),
                   m68k_get_reg(NULL, M68K_REG_A6));
        }
    }
#endif
}

/* ---- Running ---- */

static void fire_events(void)
{
    for (int ev = 0; ev < EV_COUNT; ev++) {
        if (mac.events[ev] > mac.now) {
            continue;
        }
        switch (ev) {
        case EV_VIA_T1:
        case EV_VIA_T2:
            via_timer_event(ev);
            break;
        case EV_KBD:
            mac.events[ev] = EV_NEVER;
            kbd_event();
            break;
        case EV_ADB:
            mac.events[ev] = EV_NEVER;
            adb_event();
            break;
        }
    }
}

static void run_until(uint64_t target)
{
    while (mac.now < target) {
        if (mac.halted || s_reset_pending) {
            mac.now = target;
            break;
        }
        uint64_t next = target;
        for (int ev = 0; ev < EV_COUNT; ev++) {
            if (mac.events[ev] < next) {
                next = mac.events[ev];
            }
        }
        if (next > mac.now) {
            mac.slice = (int)(next - mac.now);
            mac.cpu_running = true;
            int ran = m68k_execute(mac.slice);
            mac.cpu_running = false;
            mac.now += ran > 0 ? ran : 1;
        }
        fire_events();
        while (mac.now >= mac.next_second) {
            mac.next_second += MAC_CPU_HZ;
            via_one_second();
        }
    }
}

void mac_run_frame(void)
{
    if (s_reset_pending) {
        mac_reset();
    }
    mouse_frame();
    if (mac.adb) {
        adb_frame();
    } else {
        kbd_frame();
    }
    run_until(mac.frame_start + (uint64_t)MAC_SCREEN_HEIGHT * LINE_CYCLES);
    via_vbl();
    run_until(mac.frame_start + FRAME_CYCLES);

    sound_frame_end();
    sony_frame();
    mac.frame_start += FRAME_CYCLES;
    mac.frames++;
    if (mac.pram_dirty) {
        mac.pram_dirty = false;
        if (mac.pram_changed) {
            mac.pram_changed();
        }
    }
}

void mac_interrupt_switch(void)
{
    mac.nmi = true;
    mac_update_irq();
    mac.nmi = false;        /* the CPU latches level 4 at the next instruction */
}

void mac_reset(void)
{
    s_reset_pending = false;
    for (int ev = 0; ev < EV_COUNT; ev++) {
        mac.events[ev] = EV_NEVER;
    }
    mac.frame_start = mac.now;
    mac.next_second = mac.now + MAC_CPU_HZ;
    mac.halted = false;
    mac.via_irq = mac.scc_irq = mac.nmi = false;
    memset(mac.ram, 0, mac.ram_size);

    mac.overlay = false;
    mac_set_overlay(true);
    via_reset();
    scc_reset();
    iwm_reset();
    scsi_reset();
    rtc_reset();
    kbd_reset();
    adb_reset();
    input_reset();
    sound_reset();
    sony_reset();
    mac_update_irq();
    m68k_pulse_reset();
}

bool mac_init(const mac_config_t *config)
{
    memset(&mac, 0, sizeof(mac));
    const char *name = mac_rom_identify(config->rom, config->rom_size, &mac.model);
    if (name == NULL || config->rom_copy == NULL) {
        MAC_LOGW("not a ROM this emulator knows (checksum %02x%02x%02x%02x)",
                 config->rom[0], config->rom[1], config->rom[2], config->rom[3]);
        return false;
    }
    uint32_t ram = config->ram_size;
    if (ram == 0 || (ram & (ram - 1)) != 0 || ram > 4 * 1024 * 1024) {
        MAC_LOGW("RAM size %lu: needs 1, 2 or 4 MB", (unsigned long)ram);
        return false;
    }
    mac.adb = mac.model != MAC_MODEL_PLUS;
    mac.ram = config->ram;
    mac.ram_size = ram;
    mac.ram_mask = ram - 1;
    mac.rom = config->rom_copy;
    mac.rom_size = config->rom_size;
    mac.rom_mask = config->rom_size - 1;
    mac.pram = config->pram;
    if (mac.pram[0x10] != 0xA8 || mac.pram[0x0C] != 'B') {
        mac_pram_defaults(mac.pram, mac.model);
        mac.pram_dirty = true;
    }
    mac.pram_changed = config->pram_changed;
    mac.disk_ejected = config->disk_ejected;

    if (!rom_prepare(config->rom, config->rom_size)) {
        return false;
    }
    MAC_LOGI("%s, %lu KB RAM", name, (unsigned long)(ram / 1024));

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    if (config->opcode_tables != NULL) {
        m68k_use_table_copies(config->opcode_tables);
    }
    m68k_set_int_ack_callback(int_ack);
    m68k_set_reset_instr_callback(reset_instruction);
    mac_reset();
    return true;
}

void mac_get_status(mac_status_t *status)
{
    status->frames = mac.frames;
    status->pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
    status->sr = m68k_get_reg(NULL, M68K_REG_SR);
    status->ticks = mac.overlay ? 0 : mac_peek32(LM_TICKS);
    status->halted = mac.halted;
}
