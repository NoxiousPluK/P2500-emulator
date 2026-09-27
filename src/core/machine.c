#include "machine.h"
#include <stdio.h>
#include <string.h>

/* Port $05 bank latch, bit 3: set (as in $0F) banks the EPROM out and
 * exposes RAM at $0000-$0FFF instead; clear (as in $07, $00) exposes the
 * EPROM. See TODO.md T1. */
#define P2500_BANK_RAM_LOW_BIT 0x08

bool p2500_video_window_selected(const P2500Machine *m) {
    return (m->bank & P2500_BANK_VIDEO_MASK) == 0;
}

uint8_t p2500_peek(const P2500Machine *m, uint16_t addr) {
    if (addr < P2500_EPROM_SIZE && (m->bank & P2500_BANK_RAM_LOW_BIT) == 0)
        return m->eprom[addr];
    if (addr >= P2500_VRAM_BASE && addr < P2500_VRAM_BASE + P2500_VRAM_SIZE &&
        p2500_video_window_selected(m))
        return m->vram[addr - P2500_VRAM_BASE];
    return m->ram[addr];
}

static uint8_t mem_read(void *userdata, uint16_t addr) {
    P2500Machine *m = (P2500Machine *)userdata;
    return p2500_peek(m, addr);
}

static void mem_write(void *userdata, uint16_t addr, uint8_t value) {
    P2500Machine *m = (P2500Machine *)userdata;
    /* Writes to $0000-$0FFF always land in RAM - the EPROM is read-only and
     * selected on read only, which is what lets the ROM's own RAM test run
     * over its own address range without erasing itself (TODO.md T1). */
    if (addr >= P2500_VRAM_BASE && addr < P2500_VRAM_BASE + P2500_VRAM_SIZE &&
        p2500_video_window_selected(m)) {
        m->vram[addr - P2500_VRAM_BASE] = value;
        return;
    }
    m->ram[addr] = value;
}

/* IM2 CALLs the word stored at (I<<8)|vector - here that word is usually
 * the address of a small fixed "JP nn" trampoline (e.g. $FE04/$FE07 for
 * the IPL's own table at page $FE), whose *operand* is the real,
 * sometimes runtime-patched destination (see ../README.md and
 * ../../ROM Dumps/CPU-Card-Boot-EPROM/disassembly/findings.md).
 *
 * The table page is NOT a fixed constant: the IPL sets I=$FE, but once
 * control passes into CP/M's own CBIOS (Phase 2, TODO.md ISSUE-4's
 * postscript) it sets up its own IM2 table at I=$FF instead - confirmed
 * live (SYSPBI.PHI's own RECALIBRATE interrupt was being silently
 * suppressed here when this was still hardcoded to $FE). So this reads
 * I live off the CPU on every delivery instead of assuming one program's
 * choice of page.
 *
 * The only remaining gate (TODO.md T11): I must be genuinely initialized
 * to *some* real table page, not the z80 core's power-on reset value of
 * $00. Until whichever program is running executes "LD A,nn / LD I,A",
 * I is still $00, so (I<<8)|vector points into whatever happens to be at
 * the very start of RAM - the reset vector table, in-progress boot code,
 * etc. Delivering there hijacks flow into memory that was never meant to
 * be an interrupt handler at all.
 *
 * Earlier versions of this function also suppressed delivery into the
 * ROM's $03DB no-op stub or into unpatched/zeroed memory ($FF/$00) - a
 * workaround for the phantom CTC model over-firing before T3/T4 replaced
 * it with real PIO/DMA models. Now that interrupts only fire from real,
 * edge-triggered hardware conditions (pio.c's evaluate_interrupt, dma.c's
 * command completion), that workaround is gone; if a target still looks
 * wrong it means an upstream model is wrong, not something to paper over
 * here. */
static bool resolve_im2_target(P2500Machine *m, uint8_t vector, uint16_t *call_target_out,
                                uint16_t *final_dest_out) {
    *call_target_out = 0;
    *final_dest_out = 0;
    if (m->cpu.i == 0x00) return false;
    uint16_t slot_addr = (uint16_t)((m->cpu.i << 8) | vector);
    uint16_t call_target = (uint16_t)(p2500_peek(m, slot_addr) |
                                      (p2500_peek(m, (uint16_t)(slot_addr + 1)) << 8));
    uint16_t final_dest = call_target;
    if (p2500_peek(m, call_target) == 0xC3) { /* JP nn */
        final_dest = (uint16_t)(p2500_peek(m, (uint16_t)(call_target + 1)) |
                                 (p2500_peek(m, (uint16_t)(call_target + 2)) << 8));
    }
    *call_target_out = call_target;
    *final_dest_out = final_dest;
    return true;
}

/* The uPD765's own INT line doesn't drive the CPU directly - it's wired
 * into a Z80A-PIO input bit (TODO.md T4), and the PIO decides whether that
 * becomes a CPU interrupt. `fdc.int_line` is the real, held pin level
 * (TODO.md T9); p2500_step() samples it into the PIO input bit every step,
 * so both interrupt-driven delivery (PIO fires on the false->true edge)
 * and direct polling (e.g. the ROM's own "IN A,($10)/RRA" at $0832) see
 * the same level a real driver would. This callback is just an edge log. */
static void fdc_interrupt_trampoline(void *userdata) {
    (void)userdata;
}

static void log_request(P2500Machine *m, int source, uint8_t vector) {
    if (!m->intctl.verbose) return;
    uint16_t call_target, final_dest;
    if (resolve_im2_target(m, vector, &call_target, &final_dest))
        fprintf(stderr, "[irq] %s requests vector=$%02X at PC=$%04X IFF1=%d "
                        "(-> $%04X -> $%04X)\n",
                p2500_intctl_name(source), vector, m->cpu.pc, m->cpu.iff1,
                call_target, final_dest);
    else
        fprintf(stderr, "[irq] %s requests vector=$%02X at PC=$%04X - I=$%02X is not yet "
                        "a real table page, holding\n",
                p2500_intctl_name(source), vector, m->cpu.pc, m->cpu.i);
}

/* Every peripheral routes its interrupt request through the daisy chain in
 * intctl.c now, instead of calling z80_gen_int() directly (TODO.md T17). A
 * request is *held* until the CPU acknowledges it, so two devices asking in
 * the same instruction no longer overwrite each other - which they did,
 * constantly and silently. p2500_step() below does the arbitration. */
static void ctc_interrupt_trampoline(void *userdata, int channel, uint8_t vector) {
    P2500Machine *m = (P2500Machine *)userdata;
    int source = P2500_INT_CTC0 + channel;
    log_request(m, source, vector);
    p2500_intctl_request(&m->intctl, source, vector);
}

static void ctc_reset_trampoline(void *userdata, int channel) {
    P2500Machine *m = (P2500Machine *)userdata;
    p2500_intctl_reset_source(&m->intctl, P2500_INT_CTC0 + channel);
}

static void pio_interrupt_trampoline(void *userdata, uint8_t vector) {
    P2500Machine *m = (P2500Machine *)userdata;
    /* pio.c fires per port, but its callback predates needing to know
     * which; port A is the only one this machine arms interrupts on (the
     * FDC INT line, TODO.md T4) and port B's I/O mask makes it inputs
     * only. Distinguish on the vector, which the two ports program
     * separately. */
    int source = (m->pio.vector_set[P2500_PIO_PORT_B] &&
                  vector == m->pio.vector[P2500_PIO_PORT_B])
                     ? P2500_INT_PIO_B : P2500_INT_PIO_A;
    log_request(m, source, vector);
    p2500_intctl_request(&m->intctl, source, vector);
}

static void pio_reset_trampoline(void *userdata, int port) {
    P2500Machine *m = (P2500Machine *)userdata;
    p2500_intctl_reset_source(&m->intctl,
        port == P2500_PIO_PORT_B ? P2500_INT_PIO_B : P2500_INT_PIO_A);
}

static void dma_interrupt_trampoline(void *userdata, uint8_t vector) {
    P2500Machine *m = (P2500Machine *)userdata;
    log_request(m, P2500_INT_DMA, vector);
    p2500_intctl_request(&m->intctl, P2500_INT_DMA, vector);
}

static void dma_reset_trampoline(void *userdata) {
    P2500Machine *m = (P2500Machine *)userdata;
    p2500_intctl_reset_source(&m->intctl, P2500_INT_DMA);
}

/* RETI, snooped off the CPU core (see the local addition in
 * vendor/superzazu_z80/z80.c): the highest-priority device currently under
 * service releases its hold on the chain. */
static void cpu_reti_trampoline(z80 *cpu) {
    P2500Machine *m = (P2500Machine *)cpu->userdata;
    p2500_intctl_reti(&m->intctl);
}

static uint8_t port_in(z80 *cpu, uint8_t port) {
    P2500Machine *m = (P2500Machine *)cpu->userdata;
    switch (port) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        return p2500_ctc_read(&m->ctc, port);
    case 0x05: {
        /* Serial input lines (TODO.md T20). CBIOS's CTC channel-1 receive
         * ISR samples bit 7 once per bit cell ($F6A9: IN A,($05) / RLA),
         * and its channel-0 transmit ISR gates on bit 6 ($F5AD: IN A,($05)
         * / BIT 6,A). Everything else reads back as a pulled-up 1 until
         * something is traced reading it - see ../Tracing/
         * P2500-predicted-wiring-from-firmware.md C4, which predicted this
         * port was readable before any code that reads it had been found. */
        uint8_t v = 0xFF;
        if (!m->serial_rxd) v &= (uint8_t)~(1u << P2500_PORT05_RXD_BIT);
        if (!m->serial_tx_ready) v &= (uint8_t)~(1u << P2500_PORT05_TX_READY_BIT);
        return v;
    }
    case 0x06:
        return p2500_keyboard_in(&m->keyboard);
    case 0x09:
        return m->crtc_index < 18 ? m->crtc_regs[m->crtc_index] : 0x00;
    case 0x0F:
        return p2500_sesam_in(&m->sesam);
    case 0x10:
        return p2500_pio_read_data(&m->pio, P2500_PIO_PORT_A);
    case 0x11:
        return p2500_pio_read_data(&m->pio, P2500_PIO_PORT_B);
    case 0x14:
        return p2500_fdc_read_status(&m->fdc);
    case 0x15:
        return p2500_fdc_read_data(&m->fdc);
    default:
        if (m->verbose_unknown_ports)
            fprintf(stderr, "[io] unhandled IN ($%02X)\n", port);
        return 0xFF;
    }
}

static void port_out(z80 *cpu, uint8_t port, uint8_t value) {
    P2500Machine *m = (P2500Machine *)cpu->userdata;
    switch (port) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        p2500_ctc_write(&m->ctc, port, value);
        break;
    case 0x04:
        p2500_serial_out(&m->serial, value);
        break;
    case 0x05:
        m->bank = value;
        if (m->verbose_unknown_ports)
            fprintf(stderr, "[io] OUT ($05) <- $%02X (bank select)\n", value);
        break;
    case 0x08:
        m->crtc_index = value;
        break;
    case 0x09:
        if (m->crtc_index < 18) m->crtc_regs[m->crtc_index] = value;
        else if (m->verbose_unknown_ports)
            fprintf(stderr, "[crtc] write $%02X to nonexistent register R%u ignored\n",
                    value, m->crtc_index);
        break;
    case 0x0F:
        p2500_sesam_out(&m->sesam, value);
        break;
    case 0x10:
        p2500_pio_write_data(&m->pio, P2500_PIO_PORT_A, value);
        break;
    case 0x11:
        p2500_pio_write_data(&m->pio, P2500_PIO_PORT_B, value);
        break;
    case 0x12: {
        /* TODO.md ISSUE-1 fix 2 / ISSUE-3: the first two times this ROM
         * arms PIO port A (the FDC's INT line) interrupts, stand in for
         * the real uPD765's post-reset unsolicited interrupts - see
         * p2500_fdc_raise_startup_interrupt's doc comment.
         *
         * Delivered as a one-shot pulse here, deliberately NOT via
         * fdc->int_line's normal held-level path (T9): that path assumes
         * whatever ISR runs will read a SENSE INTERRUPT STATUS result to
         * clear it, which is true for the first synthetic interrupt's
         * handler but not the second's (confirmed by tracing both ISRs -
         * see ISSUE-3) - holding the level for a consumer that never
         * clears it left it stuck asserted, spuriously re-firing the next
         * time PIO interrupts happened to be re-armed for something else
         * entirely. A synthetic "the interrupt was already pending"
         * event is inherently one-shot: pulse it and move on. */
        bool was_enabled = m->pio.int_enabled[P2500_FDC_PIO_PORT];
        p2500_pio_write_control(&m->pio, P2500_PIO_PORT_A, value);
        if (!was_enabled && m->pio.int_enabled[P2500_FDC_PIO_PORT] &&
            p2500_fdc_raise_startup_interrupt(&m->fdc)) {
            p2500_pio_set_input_bit(&m->pio, P2500_FDC_PIO_PORT, P2500_FDC_PIO_BIT, true);
            p2500_pio_set_input_bit(&m->pio, P2500_FDC_PIO_PORT, P2500_FDC_PIO_BIT, false);
            m->fdc.int_line = false;
        }
        break;
    }
    case 0x13:
        p2500_pio_write_control(&m->pio, P2500_PIO_PORT_B, value);
        break;
    case 0x15:
        p2500_fdc_write_data(&m->fdc, value);
        break;
    case 0x16:
        p2500_dma_write(&m->dma, value);
        break;
    case 0x0A:
        /* diagnostic/POST latch, logged only (TODO.md T15) */
        if (m->verbose_unknown_ports)
            fprintf(stderr, "[io] OUT ($%02X) <- $%02X (not modeled)\n", port, value);
        break;
    default:
        if (m->verbose_unknown_ports)
            fprintf(stderr, "[io] unhandled OUT ($%02X) <- $%02X\n", port, value);
        break;
    }
}

void p2500_init(P2500Machine *m) {
    memset(m, 0, sizeof(*m));
    z80_init(&m->cpu);
    m->cpu.read_byte = mem_read;
    m->cpu.write_byte = mem_write;
    m->cpu.port_in = port_in;
    m->cpu.port_out = port_out;
    m->cpu.userdata = m;
    m->cpu.on_reti = cpu_reti_trampoline;

    p2500_intctl_init(&m->intctl);
    m->int_offered = -1;
    m->serial_rxd = true;      /* idle mark - nothing plugged into the serial port */
    m->serial_tx_ready = true; /* handshake input idles asserted */

    p2500_sesam_init(&m->sesam, NULL, 0);

    p2500_fdc_init(&m->fdc, NULL, 0);
    m->fdc.on_interrupt = fdc_interrupt_trampoline;
    m->fdc.interrupt_userdata = m;
    m->fdc.ram = m->ram;
    m->fdc.dma = &m->dma;

    p2500_pio_init(&m->pio);
    m->pio.on_interrupt = pio_interrupt_trampoline;
    m->pio.on_int_reset = pio_reset_trampoline;
    m->pio.interrupt_userdata = m;

    p2500_dma_init(&m->dma);
    m->dma.on_interrupt = dma_interrupt_trampoline;
    m->dma.on_int_reset = dma_reset_trampoline;
    m->dma.interrupt_userdata = m;

    p2500_ctc_init(&m->ctc);
    m->ctc.on_interrupt = ctc_interrupt_trampoline;
    m->ctc.on_reset = ctc_reset_trampoline;
    m->ctc.interrupt_userdata = m;

    p2500_keyboard_init(&m->keyboard, NULL, 0);
    p2500_serial_init(&m->serial);

    m->bank = 0x07; /* EPROM visible at $0000-$0FFF, as at power-on */
    m->verbose_unknown_ports = false;
}

bool p2500_load_rom(P2500Machine *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(m->eprom, 1, P2500_EPROM_SIZE, f);
    fclose(f);
    return n == P2500_EPROM_SIZE;
}

/* CTC channel 2's CLK/TRG: a square wave at P2500_CLOCK_TICK_HZ, derived
 * from the same T-state count everything else uses. CBIOS programs the
 * channel for the rising edge ($D5), so one full period is one tick. */
static void advance_clock_strobe(P2500Machine *m, uint32_t tstates) {
    const uint32_t half_period = P2500_CPU_HZ / (2u * P2500_CLOCK_TICK_HZ);
    m->clock_strobe_phase += tstates;
    while (m->clock_strobe_phase >= half_period) {
        m->clock_strobe_phase -= half_period;
        m->clock_strobe_level = !m->clock_strobe_level;
        p2500_ctc_set_clk_trg(&m->ctc, P2500_CTC_CLOCK_TICK_CHANNEL, m->clock_strobe_level);
    }
}

/* CTC channel 3's CLK/TRG: the keyboard controller's "byte ready" strobe -
 * one falling edge per byte offered, at P2500_KEYSTROKE_HZ. Channel 3's
 * ISR ($ED2D) then reads that whole byte off port $06.
 *
 * This replaces the old `channel3_rx_ready` flag, which had the right idea
 * - only strobe when a byte really is waiting - but hung it off a channel-0
 * ZC/TO pulse that has nothing to do with the keyboard, via a chain that
 * does not exist (see ctc.h). */
static void advance_keyboard_strobe(P2500Machine *m, uint32_t tstates) {
    const uint32_t interval = P2500_CPU_HZ / P2500_KEYSTROKE_HZ;
    if (!p2500_keyboard_byte_waiting(&m->keyboard, m->cpu.cyc)) {
        m->keyboard_strobe_phase = 0;
        return;
    }
    m->keyboard_strobe_phase += tstates;
    if (m->keyboard_strobe_phase < interval) return;
    m->keyboard_strobe_phase -= interval;
    p2500_ctc_set_clk_trg(&m->ctc, P2500_CTC_KEYBOARD_CHANNEL, true);
    p2500_ctc_set_clk_trg(&m->ctc, P2500_CTC_KEYBOARD_CHANNEL, false);
}

void p2500_step(P2500Machine *m) {
    unsigned long cyc_before = m->cpu.cyc;
    z80_step(&m->cpu);
    uint32_t elapsed = (uint32_t)(m->cpu.cyc - cyc_before);

    /* Did the core take the vector we offered? z80_step() runs the
     * instruction and then process_interrupts(), which clears int_pending
     * exactly when it accepts one. If it is still set, the CPU had
     * interrupts disabled and the device is still holding /INT - which is
     * the whole point of the chain. */
    if (m->int_offered >= 0 && !m->cpu.int_pending) {
        p2500_intctl_acknowledge(&m->intctl, m->int_offered);
        m->int_offered = -1;
    }

    p2500_pio_set_input_bit(&m->pio, P2500_FDC_PIO_PORT, P2500_FDC_PIO_BIT, m->fdc.int_line);
    p2500_ctc_tick(&m->ctc, elapsed);
    p2500_ctc_set_clk_trg(&m->ctc, P2500_CTC_SERIAL_RX_CHANNEL, m->serial_rxd);
    advance_clock_strobe(m, elapsed);
    advance_keyboard_strobe(m, elapsed);

    /* Offer the chain's winner to the core. Re-offer if a higher-priority
     * device has since asked, or withdraw if the offered one gave up: the
     * core's single slot is the bus, and only one device drives it. */
    int winner = (m->cpu.i == 0x00) ? -1 : p2500_intctl_pending(&m->intctl);
    if (winner != m->int_offered) {
        if (winner >= 0) {
            z80_gen_int(&m->cpu, m->intctl.vector[winner]);
        } else {
            m->cpu.int_pending = 0;
        }
        m->int_offered = winner;
    }

    m->total_instructions++;
}
