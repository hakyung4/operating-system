// intr.h - Interrupt management
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _INTR_H_
#define _INTR_H_

#include "riscv.h"
#include "plic.h"

// EXPORTED CONSTANT DEFINITIONS
//

#define INTR_PRIO_MIN PLIC_PRIO_MIN
#define INTR_PRIO_MAX PLIC_PRIO_MAX
#define INTR_SRC_CNT PLIC_SRC_CNT

// EXPORTED FUNCTION DECLARATIONS
// 

extern void intrmgr_init(void);
extern char intrmgr_initialized;

extern void enable_intr_source (
    int srcno, int prio,
    void (*isr)(int srcno, void * aux),
    void * isr_aux);

extern void disable_intr_source(int srcno);

extern void handle_smode_interrupt(unsigned int cause);

static inline long enable_interrupts(void) {
    return csrrsi_sstatus_SIE();
}

static inline long disable_interrupts(void) {
    return csrrci_sstatus_SIE();
}

static inline void restore_interrupts(int prev_state) {
    csrwi_sstatus_SIE(prev_state);
}

static inline int interrupts_enabled(void) {
    return ((csrr_sstatus() & RISCV_SSTATUS_SIE) != 0);
}

static inline int interrupts_disabled(void) {
    return ((csrr_sstatus() & RISCV_SSTATUS_SIE) == 0);
}

extern void intr_install_isr (
    int srcno,
    void (*isr)(int srcno, void * aux),
    void * isr_aux);
#endif // _INTR_H_