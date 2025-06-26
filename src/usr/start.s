# start.s - User application startup
#
# Copyright (c) 2024-2025 University of Illinois
# SPDX-License-identifier: NCSA
#

        .text

        .global _start
        .type   _start, @function

        .equ    USER_HEAP_START, 0xE0000000
        .equ    USER_HEAP_END,   0xF0000000
        
_start:
        addi    sp, sp, -16
        sd      a0, 0(sp)
        sd      a1, 8(sp)     

        li      a0, USER_HEAP_START
        li      a1, USER_HEAP_END
        call    heap_init

        ld      a0, 0(sp)
        ld      a1, 8(sp)
        addi    sp, sp, 16
        
        la      ra, _exit
        j       main
        .end
