/*! @file syscall.c
    @brief system call handlers 
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/



#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "scnum.h"
#include "process.h"
#include "memory.h"
#include "io.h"
#include "device.h"
#include "fs.h"
#include "intr.h"
#include "timer.h"
#include "error.h"
#include "thread.h"
#include "process.h"
#include "ioimpl.h"
// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame * tfr); // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame * tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char ** argv);
static int sysfork(const struct trap_frame * tfr);
static int syswait(int tid);
static int sysprint(const char * msg);
static int sysusleep(unsigned long us);

static int sysdevopen(int fd, const char * name, int instno);
static int sysfsopen(int fd, const char * name);

static int sysclose(int fd);
static long sysread(int fd, void * buf, size_t bufsz);
static long syswrite(int fd, const void * buf, size_t len);
static int sysioctl(int fd, int cmd, void * arg);
static int sysiodup(int oldfd, int newfd);
static int syspipe(int * wfdptr, int * rfdptr);

static int sysfscreate(const char* name); 
static int sysfsdelete(const char* name);
// EXPORTED FUNCTION DEFINITIONS
//



void handle_syscall(struct trap_frame * tfr) {  
    tfr->sepc += 4; // each instruction is 4 bytes, so update sepc so that it won't get stuck in the syscall loop
    tfr->a0 = syscall(tfr); // return value
}

// INTERNAL FUNCTION DEFINITIONS
//
int64_t syscall(const struct trap_frame * tfr) {
    int64_t scnum = tfr->a7;
    switch (scnum) {
        case SYSCALL_EXIT:
            return sysexit();
        case SYSCALL_EXEC:
            return sysexec(tfr->a0, tfr->a1, (char **)tfr->a2);
        case SYSCALL_FORK:
            return sysfork(tfr);
        case SYSCALL_WAIT:
            return syswait(tfr->a0);
        case SYSCALL_PRINT:
            return sysprint((const char *)tfr->a0);
        case SYSCALL_USLEEP:
            return sysusleep((unsigned long)tfr->a0);
        case SYSCALL_DEVOPEN:
            return sysdevopen(tfr->a0, (const char *)tfr->a1, tfr->a2);
        case SYSCALL_FSOPEN:
            return sysfsopen(tfr->a0, (const char *)tfr->a1);
        case SYSCALL_FSCREATE:
            return sysfscreate((const char *)tfr->a0);
        case SYSCALL_FSDELETE:
            return sysfsdelete((const char *)tfr->a0);
        case SYSCALL_CLOSE:
            return sysclose(tfr->a0);
        case SYSCALL_READ:
            return sysread(tfr->a0, (void *)tfr->a1, (size_t)tfr->a2);
        case SYSCALL_WRITE:
            return syswrite(tfr->a0, (const void *)tfr->a1, (size_t)tfr->a2);
        case SYSCALL_IOCTL:
            return sysioctl(tfr->a0, tfr->a1, (void *)tfr->a2);
        case SYSCALL_PIPE:
            return syspipe((int *)tfr->a0, (int *)tfr->a1);
        case SYSCALL_IODUP:
            return sysiodup(tfr->a0, tfr->a1);
        default:
            return -ENOTSUP;
    }
}

int sysexit(void) {
    process_exit();
    return 0; 
}

int sysexec(int fd, int argc, char ** argv) {
    if (fd < 0 || fd >= PROCESS_IOMAX) {
        return -EBADFD;
    }
    // for (int i = 0; i < argc; i++) {
    //     if (argv[i] == NULL) {
    //         return -EINVAL;
    //     }
    //     if (validate_vstr(argv[i], PTE_U | PTE_R) != 0) {
    //         return -EACCESS;
    //     }
    // }
    struct process* proc = current_process();
    struct io* io = proc->iotab[fd];
    if (io == NULL) {
        return -EBADFD;
    }
    return process_exec(io, argc, argv); // exec the process
}

int sysfork(const struct trap_frame * tfr) {
    return process_fork(tfr); 
}

int syswait(int tid) {
    trace("%s(%d)", __func__, tid);
    if (0 <= tid) {
        return thread_join(tid);
    } else {
        return -EINVAL;
    }
}

int sysprint(const char * msg) {
    // int result;
    // for debug
    trace("%s(msg=%p)", __func__, msg);
    // result = validate_vstr(msg, PTE_U);
    // if (result != 0) {
    //     return result;
    // }
    kprintf("Thread <%s:%d> says: %s\n",thread_name(running_thread()), running_thread(), msg);
    return 0;
}

int sysusleep(unsigned long us) {
    sleep_us(us);
    return 0;
}

int sysdevopen(int fd, const char * name, int instno) {
    if (name == NULL) {
        return -EINVAL;
    }
    // int result = validate_vstr(name, PTE_U|PTE_R);
    // if (result != 0) {
    //     return result;
    // }
    struct io* io = NULL;
    int result = open_device(name, instno, &io);
    if (result != 0) {
        // device does not exist
        return result;
    }
    struct process* proc = current_process();
    int target_fd = -1;
    if (fd == -1) {
        // find the first available fd
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            if (proc->iotab[i] == NULL) {
                target_fd = i;
                break;
            }
        }
        // if no available fd, return error
        if (target_fd == -1) {
            ioclose(io);
            return -EMFILE;
        }
    } else {
        // use the given fd
        if (fd < 0 || fd >= PROCESS_IOMAX || proc->iotab[fd] != NULL) {
            ioclose(io);
            return -EBADFD;
        }
        target_fd = fd;
    }
    proc->iotab[target_fd] = io;
    return target_fd;
}

int sysfsopen(int fd, const char * name) {
    if (name == NULL) {
        return -EINVAL;
    }
    // int result = validate_vstr(name, PTE_U|PTE_R);
    // if (result != 0) {
    //     return result;
    // }
    // check if the file exists
    struct io* io = NULL;
    int result = fsopen(name, &io);
    if (result != 0) {
        // file does not exist
        return result;
    }
    struct process* proc = current_process();
    int target_fd = -1;
    if (fd == -1) {
        // find the first available fd
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            if (proc->iotab[i] == NULL) {
                target_fd = i;
                break;
            }
        }
        // if no available fd, return error
        if (target_fd == -1) {
            ioclose(io);
            return -EMFILE;
        }
    } else {
        // use the given fd
        if (fd < 0 || fd >= PROCESS_IOMAX || proc->iotab[fd] != NULL) {
            ioclose(io);
            return -EBADFD;
        }
        target_fd = fd;
    }
    proc->iotab[target_fd] = io;
    return target_fd; 
}

int sysclose(int fd) {
    if (fd < 0 || fd >= PROCESS_IOMAX) {
        return -EBADFD;
    }
    struct process* proc = current_process();
    struct io* io = proc->iotab[fd];
    if (io == NULL) {
        return -EBADFD;
    }
    // close file/dev
    ioclose(io);
    // mark fd as used
    proc->iotab[fd] = NULL;
    return 0;
}

long sysread(int fd, void * buf, size_t bufsz) {
    if (buf == NULL) {
        return -EINVAL;
    }
    if (fd < 0 || fd >= PROCESS_IOMAX || bufsz == 0) {
        return -EBADFD;
    }
    // kernel writes to memory so we check W flag
    // if (validate_vptr(buf, bufsz, PTE_U | PTE_W) != 0) {
    //     return -EACCESS;
    // }
    struct process* proc = current_process();
    struct io* io = proc->iotab[fd];
    if (io == NULL) {
        return -EBADFD;
    }
    if (io->intf == NULL || io->intf->read == NULL) {
        return -ENOTSUP;
    }
    return ioread(io, buf, bufsz); // read from file/dev
}

long syswrite(int fd, const void * buf, size_t len) {
    if (fd < 0 || fd >= PROCESS_IOMAX ) {
        return -EBADFD;
    }
    if (buf == NULL || len == 0) {
        return 0;
    }
    // kernel reads from the memory so we check R flag
    // if (validate_vptr(buf, len, PTE_U | PTE_R) != 0) {
    //     return -EACCESS;
    // }
    struct process* proc = current_process();
    struct io* io = proc->iotab[fd];
    if (io == NULL) {
        return -EBADFD;
    }
    if (io->intf == NULL || io->intf->write == NULL) {
        return -ENOTSUP;
    }
    return iowrite(io, buf, len); // write to file/dev
}

int sysioctl(int fd, int cmd, void * arg) {
    if (fd < 0 || fd >= PROCESS_IOMAX) {
        return -EBADFD;
    }
    struct process* proc = current_process();
    struct io* io = proc->iotab[fd];
    if (io == NULL) {
        return -EBADFD;
    }
    if (io->intf == NULL || io->intf->cntl == NULL) {
        return -ENOTSUP;
    }
    return ioctl(io, cmd, arg);
}

int syspipe(int *wfdptr, int *rfdptr)
{
    if (!wfdptr || !rfdptr)   
        return -EBADFD;

    int wfd = *wfdptr;
    int rfd = *rfdptr;
    struct process *proc = current_process();

    if (wfd == rfd && wfd >= 0) {
        return -EBADFD;
    }                
    if ((wfd >= PROCESS_IOMAX) || (rfd >= PROCESS_IOMAX)) {
        return -EBADFD;
    }

    if (wfd < 0) {
        wfd = -1;
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            if (!proc->iotab[i]) { 
                wfd = i; 
                break; 
            }
        }
        if (wfd < 0) {
            return -EMFILE;
        }                       
    } else if (proc->iotab[wfd]) {     
        return -EBADFD;
    }

    if (rfd < 0) {
        rfd = -1;
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            if (i == wfd) {
                continue;     
            }    
            if (!proc->iotab[i]) { 
                rfd = i; 
                break; 
            }
        }
        if (rfd < 0)  {
            return -EMFILE;
        }                     
    } else if (proc->iotab[rfd]) {
        return -EBADFD;
    }

    if (wfd == rfd) {
        return -EBADFD;
    }                        


    struct io *wio = NULL, *rio = NULL;
    create_pipe(&wio, &rio);
    if (!wio || !rio) {                     
        if (wio) {
            ioclose(wio);
        }
        if (rio) {
            ioclose(rio);
        }
        return -EMFILE;
    }

    proc->iotab[wfd] = wio;
    proc->iotab[rfd] = rio;

    *wfdptr = wfd;
    *rfdptr = rfd;
    return 0;
}

int sysfscreate(const char* name) {
    if (name == NULL) {
        return -EINVAL;
    }
    // int result = validate_vstr(name, PTE_U|PTE_R);
    // if (result != 0) {
    //     return result;
    // }
    // check if the file exists
    struct io* dummy = NULL;
    if (fsopen(name, &dummy) == 0) {
        // file exists
        if (dummy != NULL) {
            ioclose(dummy);
        }
        return -EBUSY;
    }
    return fscreate(name); 
}

int sysfsdelete(const char* name) {
    if (name == NULL) {
        return -EINVAL;
    }
    // int result = validate_vstr(name, PTE_U|PTE_R);
    // if (result != 0) {
    //     return result;
    // }
    // check if the file exists
    struct io* dummy = NULL;
    if (fsopen(name, &dummy) != 0) {
        // file does not exist
        return -ENOENT;
    }
    ioclose(dummy);
    return fsdelete(name); 
}

int sysiodup(int oldfd, int newfd)
{
    struct process *proc = current_process();

    if (oldfd < 0 || oldfd >= PROCESS_IOMAX) {
        return -EBADFD;
    }
    struct io *oldio = proc->iotab[oldfd];
    if (!oldio) {
        return -EBADFD;
    }

    if (newfd == oldfd) {
        return oldfd;
    }

    if (newfd == -1) {
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            if (!proc->iotab[i]) { 
                newfd = i; break;
            }
        }
        if (newfd == -1) {
            return -EMFILE;
        }
    }

    if (newfd < 0 || newfd >= PROCESS_IOMAX) {
        return -EBADFD;
    }

    if (proc->iotab[newfd]) {
        ioclose(proc->iotab[newfd]);
        proc->iotab[newfd] = NULL;
    }

    proc->iotab[newfd] = ioaddref(oldio);
    return newfd;
}