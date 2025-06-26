

#ifndef _MEMORY_H_
#define _MEMORY_H_

#include "trap.h" // for struct trap_frame

#include <stddef.h>
#include <stdint.h>

// EXPORTED CONSTANTS
//

#define PAGE_ORDER 12
#define PAGE_SIZE (1UL << PAGE_ORDER)

// The ROUND_UP and ROUND_DOWN macros round argument _n_ to a multiple of _k_.

#define ROUND_UP(n,k) (((n)+(k)-1)/(k)*(k))
#define ROUND_DOWN(n,k) ((n)/(k)*(k))

// Flags for alloc_and_map_range() and set_range_flags()

#define PTE_V (1 << 0) // internal use only
#define PTE_R (1 << 1)
#define PTE_W (1 << 2)
#define PTE_X (1 << 3)
#define PTE_U (1 << 4)
#define PTE_G (1 << 5)
#define PTE_A (1 << 6) // internal use only
#define PTE_D (1 << 7) // internal use only

// EXPORTED TYPE DEFINITIONS
//

// We refer to a memory space using an opaque memory space tag

typedef unsigned long mtag_t;

// EXPORTED FUNCTION DECLARATIONS
//

extern char memory_initialized;


extern void memory_init(void);


extern mtag_t active_mspace(void);

extern mtag_t switch_mspace(mtag_t mtag);

extern mtag_t clone_active_mspace(void);

extern void reset_active_mspace(void);

extern mtag_t discard_active_mspace(void);

extern void * map_page(uintptr_t vma, void * pp, int rwxug_flags);

extern void * map_range (
    uintptr_t vma, size_t size, void * pp, int rwxug_flags);

    extern void * alloc_and_map_range (
    uintptr_t vma, size_t size, int rwxug_flags);

extern void set_range_flags(const void * vp, size_t size, int rwxug_flags);

extern void unmap_and_free_range(void * vp, size_t size);

extern void * alloc_phys_page(void);

extern void free_phys_page(void * pp);

extern void * alloc_phys_pages(unsigned int cnt);

extern void free_phys_pages(void * pp, unsigned int cnt);

extern unsigned long free_phys_page_count(void);

extern int handle_umode_page_fault (
    struct trap_frame * tfr, uintptr_t vma);

extern int validate_vptr(const void *vp, size_t len, uint_fast8_t rwxug_flags);
extern int validate_vstr(const char *vs, uint_fast8_t ug_flags);

#endif