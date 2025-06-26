// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "assert.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 

struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT];
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32];
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32];
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;
				uint32_t claim;
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);
static int plic_source_pending(uint_fast32_t srcno);
static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);
static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);
static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	// FIXME: Hardwired S-mode hart 0
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	// FIXME: Hardwired S-mode hart 0
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

// void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
// Inputs: 
//   uint_fast32_t srcno - Interrupt source number.
//   uint_fast32_t level - Priority level (0 to PLIC_PRIO_MAX).
// Outputs: 
//   None
// Description: 
//   Sets the priority level for the given interrupt source.
// Side Effects: 
//   Modifies the priority register for the given source.

static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	if(srcno < PLIC_SRC_CNT && level >= 0 && level <= PLIC_PRIO_MAX){
		PLIC.priority[srcno] = level;
	}
}

// int plic_source_pending(uint_fast32_t srcno)
// Inputs: 
//   uint_fast32_t srcno - Interrupt source number.
// Outputs: 
//   int - Returns 1 if the interrupt is pending, 0 otherwise.
// Description: 
//   Checks whether an interrupt source has a pending interrupt.
// Side Effects: 
//   Reads from the pending register.


static inline int plic_source_pending(uint_fast32_t srcno) {
	if(srcno < PLIC_SRC_CNT){
		uint32_t bit_needed = srcno % 32;
		return (PLIC.pending[srcno/32] >> bit_needed) & 1; // dividing by 32 gets you index and mod 32 gets you which bit in that index
	}
	return 0;
}

// void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: 
//   uint_fast32_t ctxno - Context number (Hart ID * 2 + privilege mode).
//   uint_fast32_t srcno - Interrupt source number.
// Outputs: 
//   None
// Description: 
//   Enables the specified interrupt source for the given context.
// Side Effects: 
//   Modifies the enable register of the PLIC.

static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	if (ctxno >= PLIC_CTX_CNT) {
		panic("Invalid ctxno in PLIC enable!");
	}
	 if (srcno < PLIC_SRC_CNT && ctxno < PLIC_CTX_CNT) {
        uint32_t bit_index = srcno % 32;
        PLIC.enable[ctxno][srcno / 32] |= (1U << bit_index); // make the pertinent bit a 1 to enable 
    }
}

// void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: 
//   uint_fast32_t ctxno - Context number (Hart ID * 2 + privilege mode).
//   uint_fast32_t srcno - Interrupt source number.
// Outputs: 
//   None
// Description: 
//   Disables the specified interrupt source for the given context.
// Side Effects: 
//   Modifies the enable register of the PLIC.

static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	if (srcid < PLIC_SRC_CNT && ctxno < PLIC_CTX_CNT) {
        uint32_t bit_index = srcid % 32;    
        PLIC.enable[ctxno][srcid / 32] &= ~(1U << bit_index); // & not in order to disable source at pertinent bit
    }
}

// void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
// Inputs: 
//   uint_fast32_t ctxno - Context number (Hart ID * 2 + privilege mode).
//   uint_fast32_t level - Threshold priority level.
// Outputs: 
//   None
// Description: 
//   Sets the priority threshold for the given context. 
//   Only interrupts with a priority higher than the threshold will be processed.
// Side Effects: 
//   Modifies the threshold register of the PLIC.

static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	if(ctxno < PLIC_CTX_CNT && level <= PLIC_PRIO_MAX && level >= PLIC_PRIO_MIN){
		PLIC.ctx[ctxno].threshold = level;
	}
}

// uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
// Inputs: 
//   uint_fast32_t ctxno - Context number (Hart ID * 2 + privilege mode).
// Outputs: 
//   uint_fast32_t - The interrupt source number that is currently pending for the given context.
// Description: 
//   Claims an interrupt for the specified context, preventing additional interrupts 
//   from being generated for that source until it is marked as completed.
// Side Effects: 
//   Reads from the claim register of the PLIC.

static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	if(ctxno < PLIC_CTX_CNT){
		return PLIC.ctx[ctxno].claim; //claim register auto holds this for us
	}
	return 0;
}

// void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: 
//   uint_fast32_t ctxno - Context number (Hart ID * 2 + privilege mode).
//   uint_fast32_t srcno - Interrupt source number.
// Outputs: 
//   None
// Description: 
//   Marks an interrupt as completed for the specified context, allowing new interrupts from that source.
// Side Effects: 
//   Writes to the claim register of the PLIC.


static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	if (!PLIC_MMIO_BASE || ctxno >= PLIC_CTX_CNT || srcno >= PLIC_SRC_CNT) {
		panic("PLIC complete access out of bounds!");
	}
	if(ctxno < PLIC_CTX_CNT && srcno < PLIC_SRC_CNT){
		PLIC.ctx[ctxno].claim = srcno;
	}
}


// void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: 
//   uint_fast32_t ctxno - Context number (Hart ID * 2 + privilege mode).
// Outputs: 
//   None
// Description: 
//   Enables all interrupt sources for the specified context.
// Side Effects: 
//   Modifies the enable registers for the context.

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	if(ctxno < PLIC_CTX_CNT){
		for(int i = 0; i < 32; i++){
			PLIC.enable[ctxno][i] = 0xFFFFFFFF; // enable every source in the spec context
		}
	}
}

// void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: 
//   uint_fast32_t ctxno - Context number (Hart ID * 2 + privilege mode).
// Outputs: 
//   None
// Description: 
//   Disables all interrupt sources for the specified context.
// Side Effects: 
//   Modifies the enable registers for the context.

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	if(ctxno < PLIC_CTX_CNT){
		for(int i = 0; i < 32; i++){
			PLIC.enable[ctxno][i] = 0x00000000; // disable every source for the context
		}
	}
}