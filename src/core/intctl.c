#include "intctl.h"
#include <stdio.h>
#include <string.h>

static const char *const source_names[P2500_INT_SOURCES] = {
    "CTC ch0", "CTC ch1", "CTC ch2", "CTC ch3", "PIO port A", "PIO port B", "DMA"
};

/* IEI/IEO order, highest priority first - see the header for why the DMA
 * has to be ahead of the PIO and why the CTC's position is still open. */
const int p2500_intctl_chain_order[P2500_INT_SOURCES] = {
    P2500_INT_DMA,
    P2500_INT_PIO_A,
    P2500_INT_PIO_B,
    P2500_INT_CTC0,
    P2500_INT_CTC1,
    P2500_INT_CTC2,
    P2500_INT_CTC3,
};

void p2500_intctl_init(P2500IntCtl *ic) {
    memset(ic, 0, sizeof(*ic));
}

const char *p2500_intctl_name(int source) {
    if (source < 0 || source >= P2500_INT_SOURCES) return "?";
    return source_names[source];
}

void p2500_intctl_request(P2500IntCtl *ic, int source, uint8_t vector) {
    if (source < 0 || source >= P2500_INT_SOURCES) return;
    ic->vector[source] = vector;
    ic->requests[source]++;
    if (ic->requested[source]) return; /* already asserted - /INT is a level */
    ic->requested[source] = true;
    if (ic->verbose)
        fprintf(stderr, "[int] %s requests vector=$%02X\n", source_names[source], vector);
}

void p2500_intctl_withdraw(P2500IntCtl *ic, int source) {
    if (source < 0 || source >= P2500_INT_SOURCES) return;
    ic->requested[source] = false;
}

void p2500_intctl_reset_source(P2500IntCtl *ic, int source) {
    if (source < 0 || source >= P2500_INT_SOURCES) return;
    if (ic->verbose && (ic->requested[source] || ic->under_service[source]))
        fprintf(stderr, "[int] %s reset/disabled - dropping request=%d under_service=%d\n",
                source_names[source], ic->requested[source], ic->under_service[source]);
    ic->requested[source] = false;
    ic->under_service[source] = false;
}

int p2500_intctl_pending(const P2500IntCtl *ic) {
    for (int i = 0; i < P2500_INT_SOURCES; i++) {
        int s = p2500_intctl_chain_order[i];
        if (ic->under_service[s]) return -1; /* it is holding IEO low downstream */
        if (ic->requested[s]) return s;
    }
    return -1;
}

void p2500_intctl_acknowledge(P2500IntCtl *ic, int source) {
    if (source < 0 || source >= P2500_INT_SOURCES) return;
    ic->requested[source] = false;
    ic->under_service[source] = true;
    ic->acknowledged[source]++;
    if (ic->verbose)
        fprintf(stderr, "[int] %s acknowledged, vector=$%02X\n",
                source_names[source], ic->vector[source]);
}

void p2500_intctl_reti(P2500IntCtl *ic) {
    for (int i = 0; i < P2500_INT_SOURCES; i++) {
        int s = p2500_intctl_chain_order[i];
        if (!ic->under_service[s]) continue;
        ic->under_service[s] = false;
        if (ic->verbose)
            fprintf(stderr, "[int] RETI releases %s\n", source_names[s]);
        return;
    }
}
