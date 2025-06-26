// excp.c - Exception handlers
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "trap.h"
#include "riscv.h"
#include "memory.h"
#include "thread.h"
#include "intr.h"
#include "console.h"
#include "assert.h"
#include "string.h"
#include "process.h"

#include <stddef.h>

// EXPORTED FUNCTION DECLARATIONS
//

// The following two functions, defined below, are called to handle an exception
// from trap.s.

extern void handle_smode_exception(unsigned int cause, struct trap_frame * tfr);
extern void handle_umode_exception(unsigned int cause, struct trap_frame * tfr);

// IMPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame * tfr); // syscall.c

// INTERNAL GLOBAL VARIABLES
//

static const char * const excp_names[] = {
	[RISCV_SCAUSE_INSTR_ADDR_MISALIGNED] = "Misaligned instruction address",
	[RISCV_SCAUSE_INSTR_ACCESS_FAULT] = "Instruction access fault",
	[RISCV_SCAUSE_ILLEGAL_INSTR] = "Illegal instruction",
	[RISCV_SCAUSE_BREAKPOINT] = "Breakpoint",
	[RISCV_SCAUSE_LOAD_ADDR_MISALIGNED] = "Misaligned load address",
	[RISCV_SCAUSE_LOAD_ACCESS_FAULT] = "Load access fault",
	[RISCV_SCAUSE_STORE_ADDR_MISALIGNED] = "Misaligned store address",
	[RISCV_SCAUSE_STORE_ACCESS_FAULT] = "Store access fault",
    [RISCV_SCAUSE_ECALL_FROM_UMODE] = "Environment call from U mode",
    [RISCV_SCAUSE_ECALL_FROM_SMODE] = "Environment call from S mode",
    [RISCV_SCAUSE_INSTR_PAGE_FAULT] = "Instruction page fault",
    [RISCV_SCAUSE_LOAD_PAGE_FAULT] = "Load page fault",
    [RISCV_SCAUSE_STORE_PAGE_FAULT] = "Store page fault"
};

// EXPORTED FUNCTION DEFINITIONS
//

void handle_smode_exception(unsigned int cause, struct trap_frame * tfr) {
    const char * name = NULL;
    char msgbuf[80];

    if (0 <= cause && cause < sizeof(excp_names)/sizeof(excp_names[0]))
		name = excp_names[cause];
	
	if (name != NULL) {
        switch (cause) {
        case RISCV_SCAUSE_LOAD_PAGE_FAULT:
        case RISCV_SCAUSE_STORE_PAGE_FAULT:
        case RISCV_SCAUSE_INSTR_PAGE_FAULT:
        case RISCV_SCAUSE_LOAD_ADDR_MISALIGNED:
        case RISCV_SCAUSE_STORE_ADDR_MISALIGNED:
        case RISCV_SCAUSE_INSTR_ADDR_MISALIGNED:
        case RISCV_SCAUSE_LOAD_ACCESS_FAULT:
        case RISCV_SCAUSE_STORE_ACCESS_FAULT:
        case RISCV_SCAUSE_INSTR_ACCESS_FAULT:
            snprintf(msgbuf, sizeof(msgbuf),
                "%s at %p for %p in S mode",
                name, (void*)tfr->sepc, (void*)csrr_stval());
            break;
        default:
            snprintf(msgbuf, sizeof(msgbuf),
                "%s at %p in S mode",
                name, (void*)tfr->sepc);
        }
    } else {
        snprintf(msgbuf, sizeof(msgbuf),
            "Exception %d at %p in S mode",
            cause, (void*)tfr->sepc);
    }
    
    panic(msgbuf);
}

void handle_umode_exception(unsigned int cause, struct trap_frame * tfr) {
    const char *name = NULL;
    char msgbuf[80];

    // Look up the exception cause
    if (0 <= cause && cause < sizeof(excp_names)/sizeof(excp_names[0]))
        name = excp_names[cause];

    if (name != NULL) {
        switch (cause) {
            // Handle system calls (e.g., ecall)
            case RISCV_SCAUSE_ECALL_FROM_UMODE:
                // Handle system call (dispatch to syscall handler)
                handle_syscall(tfr);
                return;  // If handled successfully, return without panic

            // Handle page faults (user-space memory access violations)
            case RISCV_SCAUSE_LOAD_PAGE_FAULT:
            if (handle_umode_page_fault(tfr, csrr_stval()) == 0) {
                // If the page fault could not be handled, terminate the process
                process_exit();
            }
            return;  
            case RISCV_SCAUSE_STORE_PAGE_FAULT:
            if (handle_umode_page_fault(tfr, csrr_stval()) == 0) {
                // If the page fault could not be handled, terminate the process
                process_exit();
            }
            return;  
            default:
                // If we don't know how to handle this exception, log it
                snprintf(msgbuf, sizeof(msgbuf),
                    "%s at %p in U mode",
                    name, (void*)tfr->sepc);
                break;
        }
    } else {
        snprintf(msgbuf, sizeof(msgbuf),
            "Exception %d at %p in U mode",
            cause, (void*)tfr->sepc);
    }

    // If the exception couldn't be handled gracefully, exit the process
    panic(msgbuf);
}
