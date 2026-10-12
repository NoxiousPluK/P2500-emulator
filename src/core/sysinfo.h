#ifndef P2500_SYSINFO_H
#define P2500_SYSINFO_H

#include "machine.h"
#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * "What is this machine running" - assembled in one place so the GUI panel
 * and `p2500-emu --sysinfo` cannot disagree about the answer.
 *
 * Everything here is a READ of the machine plus the symbol directory. No
 * new core state, nothing stepped, nothing latched: calling this twice in a
 * row on a paused machine gives the same answer twice, and calling it at
 * all changes nothing a guest could notice.
 *
 * The report is ordered by how well each claim can be proved, and it says
 * only what it can prove:
 *
 *   Machine     always. The ROM images, the bank latch, the CRTC geometry,
 *               the drives and what is in them. None of it depends on a
 *               guest having run.
 *   Symbols     always. How many names are loaded, and which shipped files
 *               fingerprint the machine as it stands - the dry-run half of
 *               p2500_debug_auto_symbols, so the line cannot drift from
 *               what `--symbols auto` would actually do.
 *   CP/M        only when page zero holds CP/M's vectors AND the 17-slot
 *               jump table they lead to is all `JP nn`. Then the memory map
 *               it implies, and drive A's DPB decoded - so a disk with an
 *               unusual format shows it instead of being averaged away.
 *   Devices     only when a device table can be found by shape. This is the
 *               honest answer to "how many drives does this image support":
 *               P25K_B's CBIOS publishes three DPHs and P25K_G's two, and
 *               the panel can say so rather than the front-end guessing
 *               from P2500_FDC_MAX_DRIVES.
 *
 * A machine running something that is not CP/M - the UCSD p-System disks -
 * must therefore produce a SHORT report, not a wrong one. Every section
 * past Machine is gated on its own shape test, and a failed test prints one
 * line saying the structure was not found rather than a decode of whatever
 * bytes happened to be there.
 *
 * COST, because the GUI rebuilds this every frame the panel is open and the
 * structural scans are brute-force sweeps of BIOS space: measured at
 * 0.53 ms in the worst case (CP/M booted, four images attached, so every
 * scan runs and every drive is fingerprinted), which is 2.7% of a 20 ms
 * frame. Measured rather than assumed, and the reason there is no cache
 * here: adding one would mean deciding when the answer goes stale, which
 * is a harder question than 0.53 ms is a problem.
 */

/* Why these numbers: 16 devices is the device table's own cap (its count
 * byte is shape-tested to 2..32, and more than 16 has never been seen);
 * the line budget is that plus the fixed sections, rounded up. A report
 * that would overflow is truncated rather than partially written, and the
 * caller can tell because the count it gets back equals the cap. */
#define P2500_SYSINFO_MAX_LINES 96
#define P2500_SYSINFO_LABEL 40
#define P2500_SYSINFO_VALUE 112

typedef enum {
    P2500_SYS_HEADING = 0, /* a section title; `value` is empty */
    P2500_SYS_ROW,         /* label: value */
    P2500_SYS_NOTE         /* one line of prose; `label` is empty */
} P2500SysKind;

typedef struct {
    P2500SysKind kind;
    char label[P2500_SYSINFO_LABEL];
    char value[P2500_SYSINFO_VALUE];
} P2500SysLine;

/* Fill `out` with at most `max` lines and return how many were written.
 * `d` may be NULL (then the Symbols section reports only what fingerprints
 * the machine, since there is no table to count). `symbol_dir` may be NULL
 * to skip every fingerprint lookup - which is what to pass if the caller
 * has no symbols directory, rather than a path it hopes exists. */
int p2500_sysinfo(const P2500Machine *m, const P2500Debug *d,
                  const char *symbol_dir, P2500SysLine *out, int max);

/* ------------------------------------------------- the structural probes
 *
 * These are the shape tests the report is gated on, exposed because they
 * answer questions a front-end asks for its own reasons - "does the
 * running BIOS map this drive letter" being the one that has a user
 * visible consequence (an unmapped letter gets `Bdos Err On D: Select`).
 *
 * They are ports of tools/derive_syms.py's device-table and DPH/DPB scans
 * in the research repo, which is where the symbol files came from; keeping
 * them to the same shape tests is what makes `make test` section 18 a
 * check on both at once. Deliberately in core/ and NOT in the machine
 * model: these read a guest operating system's data structures, which the
 * hardware knows nothing about. core/debug.c already reads CP/M's jump
 * table for the same reason.
 *
 * Every one returns false and writes nothing when its shape test fails. */

typedef struct {
    uint16_t jump_table;  /* CBIOS's 17 JP slots */
    uint16_t bdos_entry;  /* page zero $0006 */
    uint16_t bdos_base;   /* the entry's own page */
    uint16_t ccp_base;    /* BDOS base less $800 - CP/M's own layout */
    /* The build size in K, derived: the CCP base IS the build size, $400
     * per K, anchored at $CC00 = 58K. Both ends of that rule are read off
     * the profiles' own banner strings - `CP/M 2.2 - 58K` with a $CC00
     * base and `CP/M 2.2 - 56K` with a $C400 one (T59) - so this is a
     * two-point fit, not an extrapolation from one. `build_exact` is false
     * when the base is not a whole number of K, which no known build is;
     * the report says so rather than rounding an unexpected base away. */
    uint16_t build_k;
    bool build_exact;
} P2500CpmMap;

/* True when page zero holds CP/M's two vectors and the 17-slot jump table
 * they lead to is all `JP nn`. 17 slots of the right opcode is the test
 * that separates a CP/M BIOS from three bytes that happen to read $C3. */
bool p2500_sysinfo_cpm_map(const P2500Machine *m, P2500CpmMap *out);

typedef struct {
    uint16_t spt;              /* 128-byte sectors per track */
    uint8_t bsh, blm, exm;
    uint16_t dsm, drm, cks, off;
    uint16_t block_size;       /* 128 << bsh, derived */
    uint16_t dir_entries;      /* drm + 1, derived */
    unsigned long capacity;    /* (dsm + 1) * block_size, derived */
} P2500Dpb;

typedef struct {
    uint16_t table;    /* the first DPH */
    int count;         /* how many at stride 16 share one DPB - the number
                        * of drive letters this BIOS actually maps */
    uint16_t dpb_addr;
    uint16_t xlt, dirbuf;
    P2500Dpb dpb;
} P2500DiskMap;

/* The DPH table, found by requiring a 16-byte record whose +10 points at
 * something that parses as a DPB (BLM must be exactly 2^BSH-1, and the rest
 * in range), then taking the longest run at stride 16 sharing that DPB. */
bool p2500_sysinfo_disk_map(const P2500Machine *m, P2500DiskMap *out);

typedef struct {
    uint8_t id;
    uint16_t driver;
    uint16_t fntab;
    int functions;    /* -1 when the driver's function table was not found */
} P2500DeviceEntry;

#define P2500_SYSINFO_MAX_DEVICES 16

typedef struct {
    uint16_t table;
    int count;
    P2500DeviceEntry dev[P2500_SYSINFO_MAX_DEVICES];
} P2500DeviceMap;

/* A device table is a count byte then {id, ptr} records whose pointers all
 * land on [semaphore][driver] pairs in BIOS space, with distinct ids and at
 * least one id this project knows. A count byte followed by N well-formed
 * records is already unlikely in data; the known id is what makes it THIS
 * machine's device table rather than a coincidence. */
bool p2500_sysinfo_device_map(const P2500Machine *m, P2500DeviceMap *out);

/* The name this project has for a device id ("keyboard", "screen", ...), or
 * NULL when the id is not one of the known ones - in which case a caller
 * should print the id and nothing else rather than invent a meaning. */
const char *p2500_sysinfo_device_name(uint8_t id);

#ifdef __cplusplus
}
#endif

#endif
