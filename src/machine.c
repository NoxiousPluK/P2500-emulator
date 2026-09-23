#include "machine.h"
#include <stdio.h>
#include <string.h>

/* Port $05 bank latch, bit 3: set (as in $0F) banks the EPROM out and
 * exposes RAM at $0000-$0FFF instead; clear (as in $07, $00) exposes the
 * EPROM. See TODO.md T1. */
#define P2500_BANK_RAM_LOW_BIT 0x08

uint8_t p2500_peek(const P2500Machine *m, uint16_t addr) {
    if (addr < P2500_EPROM_SIZE && (m->bank & P2500_BANK_RAM_LOW_BIT) == 0)
        return m->eprom[addr];
    return m->ram[addr];
}

static uint8_t mem_read(void *userdata, uint16_t addr) {
    P2500Machine *m = (P2500Machine *)userdata;
    return p2500_peek(m, addr);
}

static void mem_write(void *userdata, uint16_t addr, uint8_t value) {
    P2500Machine *m = (P2500Machine *)userdata;
    m->ram[addr] = value; /* writes to $0000-$0FFF always land in RAM */
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
    uint16_t call_target = (uint16_t)(m->ram[slot_addr] | (m->ram[(uint16_t)(slot_addr + 1)] << 8));
    uint16_t final_dest = call_target;
    if (m->ram[call_target] == 0xC3) { /* JP nn */
        final_dest = (uint16_t)(m->ram[(uint16_t)(call_target + 1)] |
                                 (m->ram[(uint16_t)(call_target + 2)] << 8));
    }
    *call_target_out = call_target;
    *final_dest_out = final_dest;
    return true;
}

static void log_suppressed(P2500Machine *m, const char *source, uint8_t vector) {
    fprintf(stderr, "[irq] %s interrupt vector=$%02X - suppressed, I=$%02X not yet "
                    "initialized\n", source, vector, m->cpu.i);
}

/* Interrupt delivery relies on the z80 core's native IFF1 gating (see
 * process_interrupts in z80.c) plus resolve_im2_target's I-register check
 * above; an SP-baseline "still in flight" gate was tried and dropped -
 * this ROM's interrupt handlers exit via "LD SP,($FF26)" + JP, not
 * RET/RETI, so they never naturally unwind the stack the way that gate
 * assumed. */
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

static void pio_interrupt_trampoline(void *userdata, uint8_t vector) {
    P2500Machine *m = (P2500Machine *)userdata;
    uint16_t call_target, final_dest;
    if (!resolve_im2_target(m, vector, &call_target, &final_dest)) {
        log_suppressed(m, "PIO", vector);
        return;
    }
    fprintf(stderr, "[irq] PIO interrupt -> z80_gen_int(vector=$%02X) at PC=$%04X IFF1=%d "
                    "(-> $%04X -> $%04X)\n",
            vector, m->cpu.pc, m->cpu.iff1, call_target, final_dest);
    z80_gen_int(&m->cpu, vector);
}

static void dma_interrupt_trampoline(void *userdata, uint8_t vector) {
    P2500Machine *m = (P2500Machine *)userdata;
    uint16_t call_target, final_dest;
    if (!resolve_im2_target(m, vector, &call_target, &final_dest)) {
        log_suppressed(m, "DMA", vector);
        return;
    }
    fprintf(stderr, "[irq] DMA interrupt -> z80_gen_int(vector=$%02X) at PC=$%04X IFF1=%d "
                    "(-> $%04X -> $%04X)\n",
            vector, m->cpu.pc, m->cpu.iff1, call_target, final_dest);
    z80_gen_int(&m->cpu, vector);
}

static void ctc_interrupt_trampoline(void *userdata, uint8_t vector) {
    P2500Machine *m = (P2500Machine *)userdata;
    uint16_t call_target, final_dest;
    if (!resolve_im2_target(m, vector, &call_target, &final_dest)) {
        log_suppressed(m, "CTC", vector);
        return;
    }
    fprintf(stderr, "[irq] CTC interrupt -> z80_gen_int(vector=$%02X) at PC=$%04X IFF1=%d "
                    "(-> $%04X -> $%04X)\n",
            vector, m->cpu.pc, m->cpu.iff1, call_target, final_dest);
    z80_gen_int(&m->cpu, vector);
}

static uint8_t port_in(z80 *cpu, uint8_t port) {
    P2500Machine *m = (P2500Machine *)cpu->userdata;
    switch (port) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        return p2500_ctc_read(&m->ctc, port);
    case 0x06:
        return p2500_keyboard_in(&m->keyboard);
    case 0x09:
        return m->crtc_regs[m->crtc_index & 0x0F];
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
        m->crtc_regs[m->crtc_index & 0x0F] = value;
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

    p2500_sesam_init(&m->sesam, NULL, 0);

    p2500_fdc_init(&m->fdc, NULL, 0);
    m->fdc.on_interrupt = fdc_interrupt_trampoline;
    m->fdc.interrupt_userdata = m;
    m->fdc.ram = m->ram;
    m->fdc.dma = &m->dma;

    p2500_pio_init(&m->pio);
    m->pio.on_interrupt = pio_interrupt_trampoline;
    m->pio.interrupt_userdata = m;

    p2500_dma_init(&m->dma);
    m->dma.on_interrupt = dma_interrupt_trampoline;
    m->dma.interrupt_userdata = m;

    p2500_ctc_init(&m->ctc);
    m->ctc.on_interrupt = ctc_interrupt_trampoline;
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

void p2500_step(P2500Machine *m) {
    z80_step(&m->cpu);
    p2500_pio_set_input_bit(&m->pio, P2500_FDC_PIO_PORT, P2500_FDC_PIO_BIT, m->fdc.int_line);
    p2500_ctc_tick(&m->ctc, m->keyboard.pos < m->keyboard.queue_len);
    m->total_instructions++;
}
