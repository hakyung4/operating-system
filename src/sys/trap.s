# trap.s
#
# Copyright (c) 2024-2025 University of Illinois
# SPDX-License-identifier: NCSA
#

        .text
        .global _smode_trap_entry
        .type   _smode_trap_entry, @function
        .balign 4 # Trap entry must be 4-byte aligned

        # When entering a trap, we store all CPU state in a _trap frame_ struct,
        # which is defined as follows in trap.h:
        # 
        #   struct trap_frame {
        #       long a0, a1, a2, a3, a4, a5, a6, a7;
        #       long t0, t1, t2, t3, t4, t5, t6;
        #       long s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
        #       void * ra;
        #       void * sp;
        #       void * gp;
        #       void * tp;
        #       long sstatus;
        #       unsigned long instret;
        #       void * fp;
        #       void * sepc;
        #   };

        # We define a macro for each member's offset within the trap frame and a
        # macro for the trap frame size. Do not confuse `A5`, a constant defined
        # below, with the _a5_ register.

        .equ    A0, 0*8
        .equ    A1, 1*8 
        .equ    A2, 2*8 
        .equ    A3, 3*8 
        .equ    A4, 4*8 
        .equ    A5, 5*8 
        .equ    A6, 6*8 
        .equ    A7, 7*8
        .equ    T0, 8*8
        .equ    T1, 9*8
        .equ    T2, 10*8
        .equ    T3, 11*8
        .equ    T4, 12*8
        .equ    T5, 13*8
        .equ    T6, 14*8 
        .equ    S1, 15*8
        .equ    S2, 16*8
        .equ    S3, 17*8
        .equ    S4, 18*8
        .equ    S5, 19*8
        .equ    S6, 20*8
        .equ    S7, 21*8
        .equ    S8, 22*8
        .equ    S9, 23*8
        .equ    S10, 24*8
        .equ    S11, 25*8
        .equ    RA, 26*8
        .equ    SP, 27*8
        .equ    GP, 28*8
        .equ    TP, 29*8
        .equ    SSTATUS, 30*8
        .equ    SINSTRET, 31*8
        .equ    FP, 32*8
        .equ    SEPC, 33*8
        .equ    TFRSZ, 34*8

        # The base of every stack contains a _thread stack anchor_ struct, which
        # is defined in thread.c as:
        # 
        #   struct thread_stack_anchor {
        #       struct thread * ktp;
        #       void * kgp;
        #   };
        # 
        # This allows us to restore the S-mode _gp_ and _tp_ values.
        #

        .equ    KGP, 1*8
        .equ    KTP, 0*8

_smode_trap_entry:
#sscratch should be have ksp before sret 
#store usp into trap frame should be in sp before sret 
#save all the gpr
        # Swap _sp_ and _sscratch_. When we're in U mode, sscratch contains a
        # pointer to a trap frame at the base of the kernel stack. When we're in
        # S mode, _sscratch_ is zero.

        csrrw   sp, sscratch, sp        # sp =sscratch = kernel stack sscratch = user stack
        beqz    sp, smode_trap_entry_from_smode
smode_trap_entry_from_umode:
        # sp = kernel stack # sscratch = user stack
        addi   sp, sp, -TFRSZ        # carve space, save regs ...

        sd      tp, TP(sp)


# Save general purpose registers to trap frame(kernel stack)

        sd      a0, A0(sp)
        sd      a1, A1(sp)
        sd      a2, A2(sp)
        sd      a3, A3(sp)
        sd      a4, A4(sp)
        sd      a5, A5(sp)
        sd      a6, A6(sp)
        sd      a7, A7(sp)

        sd      t0, T0(sp)
        sd      t1, T1(sp)
        sd      t2, T2(sp)
        sd      t3, T3(sp)
        sd      t4, T4(sp)
        sd      t5, T5(sp)
        sd      t6, T6(sp)

        sd      s1, S1(sp)
        sd      s2, S2(sp)
        sd      s3, S3(sp)
        sd      s4, S4(sp)
        sd      s5, S5(sp)
        sd      s6, S6(sp)
        sd      s7, S7(sp)
        sd      s8, S8(sp)
        sd      s9, S9(sp)
        sd      s10, S10(sp)
        sd      s11, S11(sp)

        sd      ra, RA(sp)
        sd      fp, FP(sp)

        ld      tp, KTP+TFRSZ(sp)
     
        csrrw   t5, sscratch, zero
        
        sd      t5, SP(sp)

        # Save sstatus, sepc, instret
        csrr    t6, sstatus
        sd      t6, SSTATUS(sp)
        csrr    t6, sepc
        sd      t6, SEPC(sp)
        rdinstret t6
        sd      t6, SINSTRET(sp)

        

        # Set up ra to return from exception and interrupt handlers to next instruction
        call    smode_trap_entry_from_umode_cont


        ld      a0, A0(sp)
        ld      a1, A1(sp)
        ld      a2, A2(sp)
        ld      a3, A3(sp)
        ld      a4, A4(sp)
        ld      a5, A5(sp)
        ld      a6, A6(sp)
        ld      a7, A7(sp)

        ld      t1, T1(sp)
        ld      t2, T2(sp)
        ld      t3, T3(sp)
        ld      t4, T4(sp)
        ld      t5, T5(sp)

        ld      s1, S1(sp)
        ld      s2, S2(sp)
        ld      s3, S3(sp)
        ld      s4, S4(sp)
        ld      s5, S5(sp)
        ld      s6, S6(sp)
        ld      s7, S7(sp)
        ld      s8, S8(sp)
        ld      s9, S9(sp)
        ld      s10, S10(sp)
        ld      s11, S11(sp)
        
        ld      ra, RA(sp)
        ld      fp, FP(sp)

        ld      t0, T0(sp)
        ld     tp, TP(sp)            # <-- restore user TP just before returning
        

        # Restore sstatus, sepc
        ld      t6, SSTATUS(sp)
        csrw    sstatus, t6
        ld      t6, SEPC(sp)
        csrw    sepc, t6
        
        # Set up fp to look like a normal stack frame
        
        addi    t6, sp, TFRSZ
        csrw    sscratch, t6
        ld      t6, T6(sp)

        ld      sp, SP(sp)
        
        sret    # Return to user mode



smode_trap_entry_from_umode_cont:
        csrr    a0, scause      # a0 contains "exception code"
        mv      a1, sp          # a1 contains trap frame pointer
        
        bgez    a0, handle_umode_exception # in excp.c
        
        slli    a0, a0, 1       # clear msb
        srli    a0, a0, 1       #

        j       handle_umode_interrupt # in intr.c

smode_trap_entry_from_smode:

        # When we're in S mode, we continue using the kernel _sp_, _tp_, and
        # _gp_. First, recover _sp_ from sscratch and write zero to scratch to
        # indicate that we are now in S mode.

        csrrw   sp, sscratch, zero      # Get kernel SP back from sscratch
        addi    sp, sp, -TFRSZ          # Allocate trap frame

        # Save general purpose registers to trap frame

        sd      a0, A0(sp)
        sd      a1, A1(sp)
        sd      a2, A2(sp)
        sd      a3, A3(sp)
        sd      a4, A4(sp)
        sd      a5, A5(sp)
        sd      a6, A6(sp)
        sd      a7, A7(sp)
        sd      t0, T0(sp)
        sd      t1, T1(sp)
        sd      t2, T2(sp)
        sd      t3, T3(sp)
        sd      t4, T4(sp)
        sd      t5, T5(sp)
        sd      t6, T6(sp)
        sd      s1, S1(sp)
        sd      s2, S2(sp)
        sd      s3, S3(sp)
        sd      s4, S4(sp)
        sd      s5, S5(sp)
        sd      s6, S6(sp)
        sd      s7, S7(sp)
        sd      s8, S8(sp)
        sd      s9, S9(sp)
        sd      s10, S10(sp)
        sd      s11, S11(sp)
        sd      ra, RA(sp)
        sd      fp, FP(sp)

        # Capture retired instruction counter and save it in trap frame

        rdinstret       t6
        sd              t6, SINSTRET(sp)

        # Save sstatus and sepc CSRs

        csrr    t6, sstatus
        sd      t6, SSTATUS(sp)
        csrr    t6, sepc
        sd      t6, SEPC(sp)

        # Set up _fp_ to look like a normal stack frame

        addi    fp, sp, TFRSZ
        
        # Set up _ra_ to return from exception and interrupt handlers to next
        # instruction after call

        call    smode_trap_entry_from_smode_cont

        # S mode handlers return here because the call instruction above places
        # this address in _ra_ before we jump to an exception or trap handler.
        # Restore all GPRs except _gp_ and _tp_ (not saved/restored in S mode),
        # _sp_ (restored last) and _t6_ (used as temporary).
        
        ld      a0, A0(sp)
        ld      a1, A1(sp)
        ld      a2, A2(sp)
        ld      a3, A3(sp)
        ld      a4, A4(sp)
        ld      a5, A5(sp)
        ld      a6, A6(sp)
        ld      a7, A7(sp)
        ld      t0, T0(sp)
        ld      t1, T1(sp)
        ld      t2, T2(sp)
        ld      t3, T3(sp)
        ld      t4, T4(sp)
        ld      t5, T5(sp)
        ld      s1, S1(sp)
        ld      s2, S2(sp)
        ld      s3, S3(sp)
        ld      s4, S4(sp)
        ld      s5, S5(sp)
        ld      s6, S6(sp)
        ld      s7, S7(sp)
        ld      s8, S8(sp)
        ld      s9, S9(sp)
        ld      s10, S10(sp)
        ld      s11, S11(sp)
        ld      ra, RA(sp)
        ld      fp, FP(sp)

        # We need interrupts disabled after restoring _sepc_ so that it does not
        # get clobbered by another trap entry. Restore _sstatus_ first, which
        # automatically disables interrupts.

        ld      t6, SSTATUS(sp)
        csrw    sstatus, t6
        ld      t6, SEPC(sp)
        csrw    sepc, t6

        # Restore _t6_ and _sp_ last

        ld      t6, T6(sp)
        addi    sp, sp, TFRSZ

        sret    # done!

        # Execution of trap entry continues here from `call smode_trap_entry_from_smode_cont` above. Jump to
        # exception handler or interrupt handler depending on what brought us
        # here. Note that we clear the most significant bit of _scause_ before
        # passing its value to the interrupt handler.

smode_trap_entry_from_smode_cont:
        csrr    a0, scause      # a0 contains "exception code"
        mv      a1, sp          # a1 contains trap frame pointer
        
        bgez    a0, handle_smode_exception # in excp.c
        
        slli    a0, a0, 1       # clear msb
        srli    a0, a0, 1       #

        j       handle_smode_interrupt # in intr.c

# void __attribute__ ((noreturn)) trap_frame_jump(struct trap_frame * tfr);

# Restores CPU state from a trap frame as when returning to U mode. If indeed
# returning to U mode, the caller is responsible for placing the address of the
# re-entry trap frame into _sscratch_.

        .global trap_frame_jump
        .type   trap_frame_jump, @function

# args: a0 is trapframe
# a1 is pointer to thread stack anchor - sizeof(trap frame)
trap_frame_jump:

        # Start by restoring some GPRs now (_early_) and some after disabling
        # interrupts (_late_). See discussion in smode_trap_entry_from_umode.
        # The _late_ registers are _gp_, _tp_, _sp_, as well as _a0_ (points to
        # trap frame) and _t6_ (used as a temporary).

        ld      a2, A2(a0)
        ld      a3, A3(a0)
        ld      a4, A4(a0)
        ld      a5, A5(a0)
        ld      a6, A6(a0)
        ld      a7, A7(a0)
        ld      t0, T0(a0)
        ld      t1, T1(a0)
        ld      t2, T2(a0)
        ld      t3, T3(a0)
        ld      t4, T4(a0)
        ld      t5, T5(a0)
        ld      s1, S1(a0)
        ld      s2, S2(a0)
        ld      s3, S3(a0)
        ld      s4, S4(a0)
        ld      s5, S5(a0)
        ld      s6, S6(a0)
        ld      s7, S7(a0)
        ld      s8, S8(a0)
        ld      s9, S9(a0)
        ld      s10, S10(a0)
        ld      s11, S11(a0)
        ld      ra, RA(a0)
        ld      fp, FP(a0)

        # Restore _sstatus_ but make sure SIE=0 to disable interrupts

        ld      t6, SSTATUS(a0)
        andi    t6, t6, ~2 # SIE
        csrw    sstatus, t6

        # Restore _sepc_ and late-restore registers

        csrw sscratch, a1

        ld      t6, SEPC(a0)
        csrw    sepc, t6

        ld      t6, T6(a0)
        ld      gp, GP(a0)
        ld      tp, TP(a0)
        ld      sp, SP(a0)
        ld      a1, A1(a0)
        ld      a0, A0(a0)

        sret    # Punch it, Chewie!

        .end
