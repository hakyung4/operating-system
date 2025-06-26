# mmode.s - M mode services (Supervisor Execution Environment)
#
# Copyright (c) 2024-2025 University of Illinois
# SPDX-License-identifier: NCSA
#

# The code below implements M mode services:
# 
# 1. HALT, using virt test device, and
# 2. TIME, using mtime and mtimecmp MMIO registers.
# 
# To support (2), we need to handle timer interrupts in M mode.
#

        .equ    VTEST_ADDR, 0x100000
        .equ    MTCMP_ADDR, 0x2004000
        .equ    MTIME_ADDR, 0x200BFF8

        .equ    HALT_EID, 0x0A484c54
        .equ    HALT_SUCCESS_FID, 0
        .equ    HALT_FAILURE_FID, 1
        
        .equ    TIME_EID, 0x54494D45
        .equ    SET_STCMP_FID, 0

        .text
    	.global halt_success
    	.type   halt_success, @function

halt_success:
        li      a7, HALT_EID
        li      a6, HALT_SUCCESS_FID
        ecall
        ret

    	.global halt_failure
    	.type   halt_failure, @function

halt_failure:
        li      a7, HALT_EID
        li      a6, HALT_FAILURE_FID
        ecall
        ret

    	.global set_stcmp
    	.type   set_stcmp, @function

set_stcmp:
        li      a7, TIME_EID
        li      a6, SET_STCMP_FID
        ecall
        ret

        .global _mmode_trap_entry
    	.type   _mmode_trap_entry, @function

    	.balign 4 # trap entry must be 4-byte aligned
        
_mmode_trap_entry:

    	# Get a pointer to our M mode trap frame (defined below) into fp. We
    	# only need one register, so save it in mscratch.

    	csrw    mscratch, t0

	csrr	t0, mcause
	bgez	t0, mmode_excp_handler

mmode_intr_handler:

	# The only interrupt we handle in M mode is the timer interrupt, to
	# provide a virtualized timer to S mode.

	slli	t0, t0, 1
	addi	t0, t0, -7 << 1
	bnez	t0, unexp_mmode_interrupt

        # Set STIP, clear MTIE

        li      t0, 0x20        # STIP
        csrs    mip, t0
        slli    t0, t0, 2       # MTIE
        csrc    mie, t0
        
        # Restore state and return

	csrr    t0, mscratch
        mret

mmode_excp_handler:

        # The only exception we handle is an environment call from S mode or
        # M mode. For all other exceptions trapping to M mode, halt.

        addi    t0, t0, -9 # ecall from S mode
        beqz    t0, handle_ecall_from_smode
        addi    t0, t0, -2 # ecall from M mode
        bnez    t0, unexp_mmode_exception

handle_ecall_from_smode:

        # Increment PC past ecall instruction

        csrr    t0, mepc
        addi    t0, t0, 4
        csrw    mepc, t0

        # TIMER service has one function, so do not bother to check FID:
        #
        # void set_stcmp(uint64_t stcmp_new)
        #

        li      t0, TIME_EID
        bne     a7, t0, 1f
        bnez    a6, unsupported_function

        # Write stcmp_new, a uint64_t, to mtimecmp MMIO register. After this, we
        # can use a0 and a1 as temporary registers. Just need to zero a0 before
        # returning to indicate success.

        li      t0, MTCMP_ADDR
        sd      a0, (t0)

        # Depending on the value written to mtcmp, the timer interrupt may
        # already be asserted. To handle either case, we want to effect the
        # following:
        #
        # mie.MTIE <- ~mip.MTIP
        # mip.STIP <- mip.MTIP

        li      t0, 0x80 # MTIP
        csrr    a1, mip
        and     a1, a1, t0
        xor     a0, a1, t0
        csrc    mie, a1
        csrs    mie, a0
        srli    a0, a0, 2
        srli    a1, a1, 2
        csrs    mip, a1
        csrc    mip, a0

        # Restore state and return

	li      a0, 0
        csrr    t0, mscratch
        mret

        # HALT service is a private service (SBI implementation-specific) that
        # we provide. It has two functions (neither of which return):
        #
        # void halt_success(void)
        # void halt_failure(void)
        #

1:      li      t0, HALT_EID
        bne     a7, t0, 1f
        li      t0, VTEST_ADDR # virt test device register
        beqz    a6, 2f # branch if a6 == HALT_SUCCESS_FID
unexp_mmode_interrupt:
unexp_mmode_exception:
        # FAILURE
        li      t1, 0x3333
        sw      t1, (t0)
2:      # SUCCESS
        li      t1, 0x5555
        sw      t1, (t0)

1:      # Add additional M mode service handlers here

        # Fall though: handle request for unsupported function

unsupported_function:
        li      a0, -2
        csrr    t0, mscratch
        mret

        .end
