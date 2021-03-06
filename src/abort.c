#include "abort.h"
#include "relay_threads.h"
#define DIE 1
#define RELOAD 2

volatile uint32_t ABORT = 0;

void set_abort_bits(uint32_t v) {
    RELAY_ATOMIC_OR(ABORT, v);
}

void unset_abort_bits(uint32_t v) {
    v= ~v;
    RELAY_ATOMIC_AND(ABORT, v);
}

void set_aborted() {
    set_abort_bits(DIE);
}

uint32_t get_abort_val() {
    return RELAY_ATOMIC_READ(ABORT);
}

uint32_t not_aborted() {
    uint32_t v= RELAY_ATOMIC_READ(ABORT);
    return (v & DIE) == 0;
}

uint32_t is_aborted() {
    uint32_t v= RELAY_ATOMIC_READ(ABORT);
    return (v & DIE) == DIE;
}
