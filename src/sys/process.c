// process.c - user process
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//



#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "process.h"
#include "elf.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "thread.h"
#include "riscv.h"
#include "trap.h"
#include "memory.h"
#include "heap.h"
#include "error.h"

// COMPILE-TIME PARAMETERS
//


#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//


static int build_stack(void * stack, int argc, char ** argv);

static void fork_func(struct condition * forked, struct trap_frame * tfr);

// static void fork_func(struct condition * forked, struct trap_frame * tfr);

// INTERNAL GLOBAL VARIABLES
//


static struct process main_proc;


static struct process * proctab[NPROC] = {
    &main_proc
};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert (memory_initialized && heap_initialized);
    assert (!procmgr_initialized);

    main_proc.idx = 0;
    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

// its not creating new process.
int process_exec(struct io * exeio, int argc, char ** argv) {
    // kprintf("start of process exec\n");
    //(a) First any virtual memory mappings belonging to other user processes should be unmapped.
    //(b)Then a fresh 2nd level (root) page table should be created and initialized with the default mappings for a user process. 
    // (This is not required for Checkpoint 2, as in Checkpoint 2 any user
    // process will live in the ”main” memory space.)
    // (c) Next the executable should be loaded from the I/O interface provided as an argument into the
    //  mapped pages. (Hint: elf load)
    // (d) Finally, the thread associated with the process needs to be started in user-mode. 
    // (Hint: An assembly function in trap.s would be useful here)

    // context switching:
    /*
    i. Consider instructions that can transition between lower-privilege modes and higher privilege
    modes. Can you repurpose them for context switching purposes?
    ii. If you did repurpose them for context switching purposes, what CSRs would you need to
    edit so that the transition would start the thread’s start function in user-mode?
    */
    //
    if (!exeio || argc < 0) {
        return -EINVAL;
    }

    // valid input check
    // if (memory_validate_vptr_len(argv, sizeof(char*) * (argc + 1), PTE_R | PTE_U) < 0)
    // return -EACCESS;

    // for (int i = 0; i < argc; i++) {
    //     if (memory_validate_vstr(argv[i], PTE_R | PTE_U) < 0)
    //         return -EACCESS;
    // }
        
    //Copies arguments from user space to new page
    void* arg_page = alloc_phys_page();
    if (!arg_page) {
        thread_exit();
    }

    int stksz = build_stack(arg_page, argc, argv);
    if (stksz < 0) {
        free_phys_page(arg_page);
        thread_exit();
    }

    reset_active_mspace();

    void (*entry)(void);
    if (elf_load(exeio, &entry) < 0) {
        free_phys_page(arg_page);
        thread_exit();
    }
    void *user_stack_base = (void *)(UMEM_END_VMA - PAGE_SIZE);
    if (!map_page((uintptr_t)user_stack_base, arg_page, PTE_R | PTE_W | PTE_U)) {
        free_phys_page(arg_page);
        thread_exit();
    }


    // Set up trap frame for user mode
    struct trap_frame tf = {0};

    // start of user stack
    tf.sp = (void *)((uintptr_t)UMEM_END_VMA - stksz);
    tf.sepc = entry;

    tf.a0 = argc;
    tf.a1 = (uintptr_t)UMEM_END_VMA - stksz;


    tf.sstatus = (csrr_sstatus() & ~RISCV_SSTATUS_SPP & ~RISCV_SSTATUS_SIE);
    //    tf.sstatus = (csrr_sstatus() & ~RISCV_SSTATUS_SPP & ~RISCV_SSTATUS_SIE) | RISCV_SSTATUS_SPIE;

    //kprintf("ssratch:");
    //kprintf(current_stack_anchor());
    
    //kprintf("user stack pointer %p\n", tf.sp);
    trap_frame_jump(&tf, current_stack_anchor()); 
    //kprintf("[kernel] This line should NEVER print!\n");

    // We should never return
    halt_failure();
    return 0;
}

int process_fork(const struct trap_frame * tfr) {
    //kprintf("start of process fork\n");
    // Clone the current memory space
    mtag_t new_mtag = clone_active_mspace();
    if (!new_mtag) {
        return -ENOMEM;
    }
    
    // create a new process
    struct process * child_proc = kcalloc(1, sizeof(struct process));
    if (!child_proc) {
        discard_active_mspace();
        return -ENOMEM;
    }
    
    // modify proctab with child process
    for (int i = 0; i < NPROC; i++) {
        if (proctab[i] == NULL) {
            proctab[i] = child_proc;
            child_proc->idx = i;
            child_proc->mtag = new_mtag;
            break;
        }
    }

    // Copy IO table from parent
    struct process* parent_proc = current_process();
    for (int i = 0; i < PROCESS_IOMAX; i++) {
        struct io * io = parent_proc->iotab[i];
        if (io) {
            child_proc->iotab[i] = ioaddref(io);  // increments refcount
        }
    }

    struct trap_frame * child_tfr = kmalloc(sizeof(struct trap_frame));
    if (!child_tfr) {
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            ioclose(child_proc->iotab[i]);
        }
        kfree(child_proc);
        discard_active_mspace();
        return -ENOMEM;
    }

    memcpy(child_tfr, tfr, sizeof(struct trap_frame));
    child_tfr->a0 = 0; // Return value for child process is 0

    struct condition* done = kmalloc(sizeof(struct condition));
    condition_init(done, "done");

    // spawn a new thread for the child process
    int child_tid = thread_spawn("child", (void(*)(void))&fork_func, done, child_tfr);

    if (child_tid < 0) {
        return child_tid;
    }

    child_proc->tid = child_tid;
    // kprintf("child thread id %d\n", child_tid);
    thread_set_process(child_tid, child_proc);  

    // wait for child to finish
    condition_wait(done);
    kfree(done);
    ((struct trap_frame *)tfr)->a0 = child_tid; 

    kprintf("end of process fork\n");
    return child_tid;

}

void process_exit(void) {
    //trace("process_exit");
    struct process * proc = current_process();
    kprintf("exiting process with tid %d\n", proc->tid);
    if(proc->tid == 0) {
        panic("Main process exited");
    }
    
    thread_yield();

    // for (int i = 1;  i  < NPROC; i++) {
    //     if (proctab[i] != NULL) {
    //         kprintf("proctab[%d] = %d\n", i, proctab[i]->idx);
    //     }
    // }
    discard_active_mspace();
    for (int i = 0; i < PROCESS_IOMAX; i++) {
        struct io * io = proc->iotab[i];
        if (io) {
            ioclose(io);
        }
    }

    proctab[proc->idx] = NULL;

    // if (proctab[proc->idx] == NULL) {
    //     kprintf("proctab[%d] is NULL\n", proc->idx);
    // }
    // kfree(proc);

    //kprintf("am i here\n");
    thread_exit();
}

// INTERNAL FUNCTION DEFINITIONS
//

int build_stack(void * stack, int argc, char ** argv) {
    // stksz = Total size of the stack frame being constructed
    // argsz = Temporary variable to hold size of a single argument string
    size_t stksz, argsz;

    // Will be the new location (in user stack) of the argv[] array
    uintptr_t * newargv;
    char * p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc)
        return -ENOMEM;
    
    stksz = (argc+1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i])+1;
        if (PAGE_SIZE - stksz < argsz)
            return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert (stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv+argc+1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i])+1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

void fork_func(struct condition * done, struct trap_frame * tfr) {
    //kprintf("fork_func: child process\n");

    switch_mspace(current_process()->mtag);
    condition_broadcast(done);

    trap_frame_jump(tfr, current_stack_anchor());
}