// intr.c - Interrupt management
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef INTR_TRACE
#define TRACE
#endif

#ifdef INTR_DEBUG
#define DEBUG
#endif

#include "intr.h"
#include "trap.h"
#include "riscv.h"
#include "assert.h"
#include "plic.h"
#include "timer.h"
#include "thread.h"

#include <stddef.h>

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char intrmgr_initialized = 0;

// INTERNAL GLOBAL VARIABLE DEFINITIONS
//

static struct {
    void (*isr)(int,void*); // isr function
    void * isr_aux; // isr auxilary var
} isrtab[NIRQ];

// INTERNAL FUNCTION DECLARATIONS
//
static void handle_interrupt(unsigned int cause);

static void handle_extern_interrupt(void);

// EXPORTED FUNCTION DEFINITIONS
//

void intrmgr_init(void) {
    trace("%s()", __func__);

    disable_interrupts(); // should not be enabled yet
    plic_init();

    // Enable timer and external interrupts
    csrw_sie(RISCV_SIE_SEIE | RISCV_SIE_STIE);

    intrmgr_initialized = 1;
}

void enable_intr_source (
    int srcno,
    int prio,
    void (*isr)(int srcno, void * aux),
    void * isr_aux)
{
    assert (0 < srcno && srcno < NIRQ);
    assert (0 < prio);

    isrtab[srcno].isr = isr;
    isrtab[srcno].isr_aux = isr_aux;
    plic_enable_source(srcno, prio);
}

void disable_intr_source(int srcno) {
    plic_disable_source(srcno);
    isrtab[srcno].isr = NULL;
    isrtab[srcno].isr_aux = NULL;
}

void handle_smode_interrupt(unsigned int cause) {
    handle_interrupt(cause);
}

void handle_umode_interrupt(unsigned int cause) {
    handle_interrupt(cause);
    
    thread_yield();
}


// INTERNAL FUNCTION DEFINITIONS
//

void handle_interrupt(unsigned int cause) {
    switch (cause) {
    case RISCV_SCAUSE_STI:
        handle_timer_interrupt();
        break;
    case RISCV_SCAUSE_SEI:
        handle_extern_interrupt();
        break;
    default:
        panic(NULL);
        break;
    }
}

void handle_extern_interrupt(void) {
    int srcno;

    srcno = plic_claim_interrupt();
    assert (0 <= srcno && srcno < NIRQ);
    
    if (srcno == 0)
        return;
    
    if (isrtab[srcno].isr == NULL)
        panic(NULL);
    
    isrtab[srcno].isr(srcno, isrtab[srcno].isr_aux);

    plic_finish_interrupt(srcno);
}