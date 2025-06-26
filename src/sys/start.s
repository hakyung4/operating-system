# start.s - Kernel startup
# 
# Copyright (c) 2024-2025 University of Illinois
# SPDX-License-identifier: NCSA
#

# Imported symbols

        .global _mmode_trap_entry # defined in see.s
        .global _smode_trap_entry # defined in trap.s

# QEMU RISC-V virt system zero-stage bootloader jumps to 0x8000'0000 to start
# kernel. The linker script kernel.ld arranges for start.s to be placed here.
 
        .section        .text.start, "xa", @progbits
        .balign         4

mmode_start:

        # Configure physical memory protection. Note that entries are in order
        # of priority and the first match applies. This means that entries do
        # not need to be mutually exclusive.
        #
        # [0,100) - No access in all modes (guard)
        # [0,8000'0000) - RW in all modes (MMIO)
        # [8000'0000,8800'0000) - RWX in all modes (RAM)
        #
        
        li      t0, (0>>2) | ((0x100>>3)-1)
        csrw    pmpaddr0, t0

        li      t0, (0>>2) | ((0x80000000>>3)-1)
        csrw    pmpaddr1, t0
        
        li      t0, (0x80000000>>2) | ((0x8000000>>3)-1)
        csrw    pmpaddr2, t0

        # pmpcfg2 = 1'00'11'111 (L,NAPOT,XWR)
        # pmpcfg1 = 1'00'11'011 (L,NAPOT,RW)
        # pmpcfg0 = 1'00'11'000 (L,NAPOT,na)
        #

        li      t0, 0x9f9b98
        csrw    pmpcfg0, t0

        # Delegate to S mode all S mode interrupts and all exceptions except
        # ecall from S mode and M mode.

        li      t0, 0xb1ff
        csrw    medeleg, t0
        li      t0, 0x222
        csrw    mideleg, t0

        # Set M mode trap handler (defined in see.s)

        la      t0, _mmode_trap_entry
        csrw    mtvec, t0

        # Enable access to cycle, time, and instret counters in S mode

        csrs    mcounteren, 7

        # Switch to S mode with M mode interrupts now enabled

        li      t0, 0x1002 # bits to clear in mstatus (MPP=0b01,SIE=0)
        li      t1, 0x0880 # bits to set in mstatus (MPP=0b01,MPIE=1)
        csrc    mstatus, t0
        csrs    mstatus, t1
        la      t0, smode_start
        csrw    mepc, t0
        mret

smode_start:

        # Set trap handler for S mode (defined in trap.s)

        la      t0, _smode_trap_entry
        csrw    stvec, t0

        # Enable access to cycle, time, and instret counters in U mode

        csrs    scounteren, 7

        # Initialize sscratch (must be zero in S mode)

        csrw    sscratch, zero

        # Set initial frame and stack pointer for main thread. Set up ra so we
        # jump to halt_failure if main returns.

        mv      fp, zero
        la      sp, _main_stack_anchor
        la      ra, halt_failure # see.s
        j       main

        .section        .data.stack, "wa", @progbits
        .balign		16
        
        .equ		MAIN_STACK_SIZE, 4096

        .global		_main_stack_lowest
        .type		_main_stack_lowest, @object
        .size		_main_stack_lowest, MAIN_STACK_SIZE

        .global		_main_stack_anchor
        .type		_main_stack_anchor, @object
        .size		_main_stack_anchor, 16

_main_stack_lowest:
        .fill   MAIN_STACK_SIZE, 1, 0xA5

_main_stack_anchor:
        .dword  0 # ktp
        .dword  0 # kgp
        .end
