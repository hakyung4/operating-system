// trap.h - Trap frame
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _TRAP_H_
#define _TRAP_H_

#include <stdint.h>

// The trap_frame structure is used to save current processor state when
// handling a trap in S mode. (For handling traps to M mode, we don't bother
// with a trap frame; see see.s.)

struct trap_frame {
    long a0, a1, a2, a3, a4, a5, a6, a7;
    long t0, t1, t2, t3, t4, t5, t6;
    long s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    void * ra;
    void * sp;
    void * gp;
    void * tp;
    long sstatus;
    unsigned long long instret;
    void * fp;    // must be here
    void * sepc;  // must be here
};

extern void trap_frame_jump(struct trap_frame * tfr, void* sscratch) __attribute__ ((noreturn));

// The following functions are called to handle interrupts and exceptions from
// trap.s. The exception handlers are defined in excp.c, and the interrupt
// handlers are defined in intr.c. Note that `handle_smode_exception` means handler
// for exceptions that occur in S mode, and `handle_umode_exception` means handler
// for exceptions that occur in U mode. In both cases, the traps are taken to
// (and handled in) S mode.

extern void handle_smode_exception(unsigned int cause, struct trap_frame * tfr);
extern void handle_umode_exception(unsigned int cause, struct trap_frame * tfr);

extern void handle_smode_interrupt(unsigned int cause);
extern void handle_umode_intr(unsigned int cause);

#endif // _TRAP_H_