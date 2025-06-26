// thread.c - Threads
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif

#include "thread.h"

#include <stddef.h>
#include <stdint.h>
#include "timer.h"
#include "assert.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "memory.h"
#include "error.h"
#include "process.h"

#include <stdarg.h>

// COMPILE-TIME PARAMETERS
//

// NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

#ifndef STACK_SIZE
#define STACK_SIZE PAGE_SIZE
#endif

// EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context {
    uint64_t s[12];
    void * ra;
    void * sp;
};

struct thread_stack_anchor {
    struct thread * ktp;
    void * kgp;
};


struct thread {
    struct thread_context ctx;  // must be first member (thrasm.s)
    int id; // index into thrtab[]
    enum thread_state state;
    const char * name;
    struct thread_stack_anchor * stack_anchor;
    void * stack_lowest;
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;
    struct process* proc;
};

// INTERNAL MACRO DEFINITIONS
// 

// Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread*)__builtin_thread_pointer())

// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.

#define set_thread_state(t,s) do { \
    debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
        (t)->name, (t)->id, \
        thread_state_name((t)->state), \
        thread_state_name(s), \
        TP->name, TP->id, \
        __func__); \
    (t)->state = (s); \
} while (0)

// INTERNAL FUNCTION DECLARATIONS
//

// Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

// Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread * thr);

// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.

static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));

// void thread_reclaim(int tid)
//
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

// struct thread * create_thread(const char * name)
//
// Creates and initializes a new thread structure. The new thread is not added
// to any list and does not have a valid context (_thread_switch cannot be
// called to switch to the new thread).

static struct thread * create_thread(const char * name);

// void running_thread_suspend(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is TP, it is marked READY and placed
// on the ready-to-run list. Note that running_thread_suspend will only return if the
// current thread becomes READY.

static void running_thread_suspend(void);

// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
static void tlappend(struct thread_list * l0, struct thread_list * l1);

static void idle_thread_func(void);

// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//

extern struct thread * _thread_swtch(struct thread * thr);

extern void _thread_startup(void);

// INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR-1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; // from start.s
extern char _main_stack_anchor[]; // from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_RUNNING,
    .stack_anchor = (void*)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit",
};

extern char _idle_stack_lowest[]; // from thrasm.s
extern char _idle_stack_anchor[]; // from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void*)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup,
    // FIXME your code goes here
    .ctx.s[8] = (uint64_t)&idle_thread_func
};

static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread
};

// EXPORTED FUNCTION DEFINITIONS
//

int running_thread(void) {
    return TP->id;
}

void thrmgr_init(void) {
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}


// int thread_spawn(const char * name, void (*entry)(void), ...)
// Inputs: 
//   const char * name - The name of the thread to be created.
//   void (*entry)(void) - Pointer to the function to be executed by the thread.
//   ... - Optional arguments for the thread entry function.
// Outputs: 
//   int - The thread ID of the newly created thread, or -EMTHR if thread creation fails.
// Description: 
//   Creates a new thread, assigns it an entry function, and places it in the ready list.
// Side Effects: 
//   Modifies the thread table and ready list.

int thread_spawn (
    const char * name,
    void (*entry)(void),
    ...)
{
    struct thread * child;
    va_list ap;
    int pie;
    int i;

    child = create_thread(name);

    if (child == NULL)
        return -EMTHR;
    set_thread_state(child, THREAD_READY);

    pie = disable_interrupts();
    tlinsert(&ready_list, child);
    restore_interrupts(pie);

    // FIXME your code goes here
    // filling in entry function arguments is given below, the rest is up to you

    child->ctx.sp = child->stack_anchor; // set it to base of stack
    child->ctx.ra = &_thread_startup; // since we will need to go to startup

    va_start(ap, entry);
    child->ctx.s[8] = (uint64_t) entry; // s0-s7 are taken for arguments so this is where the entry reference will go
    for (i = 0; i < 8; i++)
        child->ctx.s[i] = va_arg(ap, uint64_t);
    va_end(ap);
    
    return child->id;
}

// void thread_exit(void)
// Inputs: 
//   None
// Outputs: 
//   None
// Description: 
//   Terminates the currently running thread, marks it as exited, and suspends execution.
// Side Effects: 
//   May trigger a context switch.

void thread_exit(void) {
    // FIXME your code goes here
    if(TP->id == MAIN_TID){
        halt_success();
    }
    else{
        set_thread_state(TP, THREAD_EXITED);
        if(TP->parent != NULL){
            condition_broadcast(&TP->child_exit); // if this thread is a child, then we indicate that it is exiting to its parent
        }
        running_thread_suspend(); // when exiting we want to suspend the thread
    }

    // should not ever happen but just in case
    halt_failure();

}

void thread_yield(void) {
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}


// int thread_join(int tid)
// Inputs: 
//   int tid - The ID of the thread to wait for.
// Outputs: 
//   int - The ID of the joined thread or -EINVAL if the operation is invalid.
// Description: 
//   Waits for a specified thread to exit and then reclaims its resources.
// Side Effects: 
//   Suspends execution until the specified thread terminates.

int thread_join(int tid) {
    if (tid < 0 || tid >= NTHR) {
        return -EINVAL;
    }

    if (tid == 0) {
        while (1) {
            int have_children = 0;

            // Look for any child that has exited
            for (int i = 1; i < NTHR; i++) {
                struct thread * child = thrtab[i];
                if (child && child->parent == TP) {
                    have_children = 1;
                    if (child->state == THREAD_EXITED) {
                        thread_reclaim(i);
                        return i;  // Return the TID of the exited child
                    }
                }
            }

            // If no children exist at all, return -EINVAL
            if (!have_children) {
                return -EINVAL;
            }

            // Otherwise, wait on any child that hasn't exited yet
            for (int i = 1; i < NTHR; i++) {
                struct thread * child = thrtab[i];
                if (child && child->parent == TP && child->state != THREAD_EXITED) {
                    condition_wait(&child->child_exit);
                    // After waking up, we loop again to check for an exited child
                    break;
                }
            }
        }
    }

    // Now handle the case where tid != 0

    struct thread * child = thrtab[tid];

    // Validate that the given thread exists and is a child of the calling thread
    if (!child || child->parent != TP) {
        return -EINVAL;
    }

    // If the child has already exited, reclaim it and return immediately
    if (child->state == THREAD_EXITED) {
        thread_reclaim(tid);
        return tid;
    }

    condition_wait(&child->child_exit);
    thread_reclaim(tid); // basically if this thread has children, its children will become the children of its parent, then it is freed
    return tid;
}


const char * thread_name(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char * running_thread_name(void) {
    return TP->name;
}

void condition_init(struct condition * cond, const char * name) {
    tlclear(&cond->wait_list);
    cond->name = name;
}

// void condition_wait(struct condition * cond)
// Inputs: 
//   struct condition * cond - Pointer to a condition variable to wait on.
// Outputs: 
//   None
// Description: 
//   Suspends execution of the calling thread until the specified condition is broadcast.
// Side Effects: 
//   Places the calling thread in the wait list and suspends execution.

void condition_wait(struct condition * cond) {
    int pie;

    trace("%s(cond=<%s>) in <%s:%d>", __func__,
        cond->name, TP->name, TP->id);

    assert(TP->state == THREAD_RUNNING);

    // Insert current thread into condition wait list
    
    set_thread_state(TP, THREAD_WAITING); // change state
    TP->wait_cond = cond; // set wait cond
    TP->list_next = NULL; // will be inserted at tail

    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);

    running_thread_suspend(); // we want to switch to another thread in the meantime
}

// void condition_broadcast(struct condition * cond)
// Inputs: 
//   struct condition * cond - Pointer to a condition variable to broadcast.
// Outputs: 
//   None
// Description: 
//   Wakes up all threads currently waiting on the specified condition variable.
// Side Effects: 
//   Moves waiting threads to the ready list and updates their states.

void condition_broadcast(struct condition * cond) {
    int oldlevel = disable_interrupts();

    // Move all waiting threads to the ready list
    tlappend(&ready_list, &cond->wait_list);

    // Iterate through the ready list and update thread states
    struct thread * thr = ready_list.head;
    while (thr != NULL) {
        if (thr->wait_cond == cond) {
            set_thread_state(thr, THREAD_READY);
            thr->wait_cond = NULL;  // Clear wait condition
        }
        thr = thr->list_next;
    }

    restore_interrupts(oldlevel);

}

// INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void) {
    // Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}

void init_idle_thread(void) {
    // Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}

static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}

const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_RUNNING] = "RUNNING",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };

    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;

    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);

    // Make our parent thread the parent of our child threads. We need to scan
    // all threads to find our children. We could keep a list of all of a
    // thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;

    if(thr->stack_lowest){
        free_phys_page(thr->stack_lowest);
    }

    kfree(thr);
}

struct thread * create_thread(const char * name) {
    struct thread_stack_anchor * anchor;
    void * stack_page;
    struct thread * thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    // Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
    
    if (tid == NTHR)
        return NULL;
    
    // Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));
    if(!thr){
        return NULL;
    }
    
    stack_page = alloc_phys_page();
    if(!stack_page){
        kfree(thr);
        return NULL;
    }

    //old version
    // anchor = stack_page + STACK_SIZE;
    // anchor -= 1; // anchor is at base of stack

    char *base = (char*)stack_page;
    // place the anchor struct in the last bytes of the page
    anchor = (struct thread_stack_anchor*)(base + STACK_SIZE) - 1; //anchor lives at top of page

    thr->stack_lowest = stack_page;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    return thr;
}

// void running_thread_suspend(void)
// Inputs: 
//   None
// Outputs: 
//   None
// Description: 
//   Suspends the execution of the currently running thread and switches to another ready thread.
// Side Effects: 
//   Modifies the thread state and may trigger a context switch.

void running_thread_suspend(void) {
    // FIXME your code goes here
    
    //this is where we call thread_switch
    disable_interrupts();

    if(TP->state == THREAD_RUNNING){
        set_thread_state(TP, THREAD_READY);
        tlinsert(&ready_list, TP);
    }

    struct thread * switchto = tlremove(&ready_list);
    if (switchto == NULL) {
        // If no one is ready, run the idle thread
        switchto = &idle_thread;
    }

    set_thread_state(switchto, THREAD_RUNNING);

    //switch memory space (only if this is a user thread)
    if (switchto->proc != NULL) {
        switch_mspace(switchto->proc->mtag);
    }
    enable_interrupts();
    _thread_swtch(switchto);

    if(TP->state == THREAD_EXITED){
        thread_reclaim(TP->id);
    }

    
}

void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}

void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;

    if (thr == NULL)
        return;

    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }

    list->tail = thr;
}

struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;

    thr = list->head;
    
    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;
    
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

// Appends elements of l1 to the end of l0 and clears l1.

void tlappend(struct thread_list * l0, struct thread_list * l1) {
    if (l0->head != NULL) {
        assert(l0->tail != NULL);
        
        if (l1->head != NULL) {
            assert(l1->tail != NULL);
            l0->tail->list_next = l1->head;
            l0->tail = l1->tail;
        }
    } else {
        assert(l0->tail == NULL);
        l0->head = l1->head;
        l0->tail = l1->tail;
    }

    l1->head = NULL;
    l1->tail = NULL;
}

void idle_thread_func(void) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.

    for (;;) {
        // If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            thread_yield();
        
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm ("wfi");
        enable_interrupts();
    }
}


void lock_init (struct lock * lock){
    if (lock == NULL) {
        return;
    }

    lock->holder = NULL;
    lock->count = 0;
    condition_init(&lock->released, "release");
}

void lock_acquire(struct lock * lock){
    if (lock == NULL) {
        return;
    }

    int pie = disable_interrupts();

    if(lock->holder == TP){
        lock->count++;
    }
    else{
        while(lock->holder != NULL){
            condition_wait(&lock->released);
        }
        lock->holder = TP;
        lock->count = 1;
    }

    restore_interrupts(pie);
}

void lock_release(struct lock * lock){
    if (lock == NULL) {
        return;
    }
    int pie = disable_interrupts();

    if(lock->holder != TP){
        restore_interrupts(pie);
        return;
    }

    lock->count--;

    if(lock->count == 0){
        lock->holder = NULL;
        condition_broadcast(&lock->released);
    }

    restore_interrupts(pie);

}

struct process* thread_process(int tid) {
    if (tid < 0 || tid >= NTHR) {
        return NULL;
    }
    // get current thread
    struct thread *thr = thrtab[tid];
    if (!thr) {
        return NULL;
    }
    
    // pointer to thread's process struct
    return thr->proc;

}

struct process * running_thread_process() {
    // the process associated with the currently running thread
    return TP->proc;
}

void thread_set_process(int tid, struct process * proc) {
    if (tid < 0 || tid >= NTHR) {
        return;
    }
    struct thread *thr = thrtab[tid];
    if (!thr) {
        return;
    }
    thr->proc = proc;

}

void * current_stack_anchor(void) {
    kprintf("TP->stack_anchor: %p\n", TP->stack_anchor);
    return TP->stack_anchor;
}

void thread_set_anchor_ktp(void) {
    TP->stack_anchor->ktp = TP;
    TP->stack_anchor->kgp = NULL;
}

void interrupter(void) {
    struct alarm al;
    alarm_init(&al, "interrupter");

    for (;;) {
        alarm_sleep_ms(&al, 10);  // 10ms sleep triggers timer interrupt
    }
}

void start_interrupter(void) {
    thread_spawn("interrupter", &interrupter);
}