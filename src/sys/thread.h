// thread.h - A thread of execution
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _THREAD_H_
#define _THREAD_H_

struct thread; // opaque decl.

struct thread_list {
    struct thread * head;
    struct thread * tail;
};

struct condition {
    const char * name; // optional
	struct thread_list wait_list;
};

struct lock {
    struct condition released;
    struct thread * holder;       // Thread currently holding the lock
    int count;
};

// EXPORTED FUNCTION DECLARATIONS
//

extern char thrmgr_initialized;
extern void thrmgr_init(void);

// struct thread * running_thread(void)
// Returns the currently running thread (pointer to struct thread).

extern int running_thread(void);

// int thread_spawn(const char * name, void (*start)(void *), ...)
// 
// Creates and starts a new thread. Argument _name_ is the name of the thread
// (optional, may be NULL), _start_ is the thread entry point, and _arg_ is an
// argument passed to the thread. The thread is added to the runnable thread
// list. If _start_ returns, this is equivalent to calling thread_exit from
// _start_. Returns the TID of the spawned thread or a negative value on error.

extern int thread_spawn (
    const char * name,
    void (*entry)(void),
    ...);

// void thread_yield(void)
// 
// Yields the CPU to another thread and returns when the current thread is next
// scheduled to run.

extern void thread_yield(void);

// int thread_join(int tid)
//
// Waits for a child of the current thread to exit. if _tid_ is not zero, the
// function waits for the identified child of the running thread to exit.
// Otherwise, the function waits for any child of the running thread. The
// function returns the thread id of the child thrad that exited or an error.
// The thread_join function returns -EINVAL if _tid_ is not zero and the
// identified thread does not exist or is not a child of the running thread. The
// thread_join function returns -EINVAL if _tid_ is zero and the running thread
// does not have any children.

extern int thread_join(int tid);

// void thread_exit(void)
//
// Terminates the currently running thread. This function does not return.

extern void __attribute__ ((noreturn)) thread_exit(void);

// Returns the name of a thread.

extern const char * thread_name(int tid);

// Returns the name of the running thread.

extern const char * running_thread_name(void);

// void condition_init(struct condition * cond, const char * name)
//
// Initializes a condition variable. The _cond_ argument must be a pointer to a
// condition struct to initialize. The _name_ argument is the name of the
// condition variable, which may be NULL. (Names are purely a debugging
// convenience.) It is valid initialize a struct condition with all zeroes.

extern void condition_init(struct condition * cond, const char * name);

// void condition_wait(struct condition * cond)
// Suspends the current thread until a condition is signalled by another thread
// or interrupt service routine. The condition_wait function may be called with
// interrupts disabled. It will enable interrupts while the thread is suspended
// and will restore interrupt enable/disable state to its value when called.
// Note that in cases where a thread needs to wait for a condition to be
// signalled by an ISR, condition_wait() should be called with interrupts
// disabled to avoid a race condition.

extern void condition_wait(struct condition * cond);

// void condition_broadcast(struct condition * cond)

// Wakes up all threads waiting on a condition. This function may be called from
// an ISR. Calling condition_broadcast() does not cause a context switch from
// the currently running thread.
// Waiting threads are added to the ready-to-run list in the order they were
// added to the wait queue.

extern void condition_broadcast(struct condition * cond);

// lock operations

extern void lock_init (struct lock * lock);

extern void lock_acquire(struct lock * lock);

extern void lock_release(struct lock * lock);

// struct process * thread_process(int tid)
//
// Returns a pointer to the process struct of a thread's process, or NULL if the
// specified thread does not have an associated process (e.g. idle thread).
 
extern struct process * thread_process(int tid);
 
// struct process * running_thread_process(void)
//
// Returns a pointer to the process struct of the running thread. Equivalent to:
// `thread_process(running_thread())`.
 
extern struct process * running_thread_process(void);
 
// Sets a thread's associated process. The /proc/ argument may be NULL if the
// thread is a kernel thread (e.g. idle).
 
extern void thread_set_process(int tid, struct process * proc);
 
extern void * current_stack_anchor(void);
extern void thread_set_anchor_ktp(void);
extern void start_interrupter(void);
#endif // _THREAD_H_