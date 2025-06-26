// plic.h - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _PLIC_H_
#define _PLIC_H_

// These functions are called from intr.c to handle external interrupts. Do not
// call these (including plic_init) directly from other parts of the kernel.

#define PLIC_PRIO_MIN 1
#define PLIC_PRIO_MAX 7

extern void plic_init(void);

extern void plic_enable_source(int srcno, int prio);
extern void plic_disable_source(int srcno);

extern int plic_claim_interrupt(void);
extern void plic_finish_interrupt(int srcno);

#endif