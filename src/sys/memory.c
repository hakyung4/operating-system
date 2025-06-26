/*! @file memory.c
    @brief Physical and virtual memory manager    
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"
#include "conf.h"
#include "riscv.h"
#include "heap.h"
#include "console.h"
#include "assert.h"
#include "string.h"
#include "thread.h"
#include "process.h"
#include "error.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE) // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE) // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk * next; ///< Next page in list
    unsigned long pagecnt; ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags:8;
    uint64_t rsw:2;
    uint64_t ppn:44;
    uint64_t reserved:7;
    uint64_t pbmt:2;
    uint64_t n:1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2*9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1*9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0*9)) % PTE_CNT)

#define MIN(a,b) (((a)<(b))?(a):(b))

#define ROUND_UP(n,k) (((n)+(k)-1)/(k)*(k)) 
#define ROUND_DOWN(n,k) ((n)/(k)*(k))

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) \
                             >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//



static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte * root, unsigned int asid);
static inline struct pte * mtag_to_ptab(mtag_t mtag);
static inline struct pte * active_space_ptab(void);

static inline void * pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void * p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk * free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
// 

void memory_init(void) {
    const void * const text_start = _kimg_text_start;
    const void * const text_end = _kimg_text_end;
    const void * const rodata_start = _kimg_rodata_start;
    const void * const rodata_end = _kimg_rodata_end;
    const void * const data_start = _kimg_data_start;
    
    void * heap_start;
    void * heap_end;

    uintptr_t pma;
    const void * pp;

    trace("%s()", __func__);

    assert (RAM_START == _kimg_start);

    kprintf("           RAM: [%p,%p): %zu MB\n",
        RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    kprintf("  Kernel image: [%p,%p)\n", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)
    
    if (MEGA_SIZE < _kimg_end - _kimg_start)
        panic(NULL);

    // Initialize main page table with the following direct mapping:
    // 
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB
    
    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void*)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP (
            HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("out of memory");
    
    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    kprintf("Heap allocator: [%p,%p): %zu KB free\n",
        heap_start, heap_end, (heap_end - heap_start) / 1024);
    
    // TODO: Initialize the free chunk list here
    // Initialize free chunk list to point to the end of the heap
    free_chunk_list = (struct page_chunk*)heap_end;
    uintptr_t free_start = (uintptr_t)heap_end;
    uintptr_t free_end = (uintptr_t)RAM_END;

    free_chunk_list->next = NULL;
    free_chunk_list->pagecnt = (free_end - free_start) / PAGE_SIZE;
    
    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}


mtag_t active_mspace(void) {
    return active_space_mtag();
}

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;
    
    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

mtag_t clone_active_mspace(void)
{
    struct pte *old_pt2 = active_space_ptab();
    struct pte *new_pt2 = alloc_phys_page();
    if (!new_pt2)
        return 0;                         

    memset(new_pt2, 0, PAGE_SIZE);

    // walk L2 (root) 
    for (int i2 = 0; i2 < PTE_CNT; i2++) {
        struct pte pte2 = old_pt2[i2];
        if (!PTE_VALID(pte2))
            continue;

        // share gigapage or global entry as‑is
        if (PTE_LEAF(pte2) || PTE_GLOBAL(pte2)) {
            new_pt2[i2] = pte2;
            continue;
        }

        // allocate new level‑1 table 
        struct pte *old_pt1 = (struct pte *)pageptr(pte2.ppn);
        struct pte *new_pt1 = alloc_phys_page();
        if (!new_pt1) goto oom;
        memset(new_pt1, 0, PAGE_SIZE);
        new_pt2[i2] = ptab_pte(new_pt1, 0);

        // walk L1 
        for (int i1 = 0; i1 < PTE_CNT; i1++) {
            struct pte pte1 = old_pt1[i1];
            if (!PTE_VALID(pte1))
                continue;

            // share megapage or global entry
            if (PTE_LEAF(pte1) || PTE_GLOBAL(pte1)) {
                new_pt1[i1] = pte1;
                continue;
            }

            // allocate new level‑0 table
            struct pte *old_pt0 = (struct pte *)pageptr(pte1.ppn);
            struct pte *new_pt0 = alloc_phys_page();
            if (!new_pt0) goto oom;
            memset(new_pt0, 0, PAGE_SIZE);
            new_pt1[i1] = ptab_pte(new_pt0, 0);

            // walk L0 (4 KiB leafs) 
            for (int i0 = 0; i0 < PTE_CNT; i0++) {
                struct pte pte0 = old_pt0[i0];
                if (!PTE_VALID(pte0))
                    continue;

                if (PTE_GLOBAL(pte0)) {
                    new_pt0[i0] = pte0;
                } else {
                    void *old_page = pageptr(pte0.ppn);
                    void *new_page = alloc_phys_page();
                    if (!new_page) goto oom;
                    memcpy(new_page, old_page, PAGE_SIZE);
                    new_pt0[i0] = leaf_pte(new_page, pte0.flags & 0xFF);
                }
            }
        }
    }

    return ptab_to_mtag(new_pt2, 0);

oom:
    panic("clone_active_mspace: out of memory");
    return 0;
}

void reset_active_mspace(void) {
    // get the root page table
    struct pte* pt2 = active_space_ptab();
    // iterate thru all entries in the root page table
    for (int i2 = 0; i2 < PTE_CNT; i2++) {
        // make a local copy
        struct pte pte2 = pt2[i2];
        // skip invalid entries
        if (!PTE_VALID(pte2) || PTE_LEAF(pte2)) {
            continue;
        }
        // iterate thru all entries in the level 1 page table
        struct pte* pt1 = (struct pte*)pageptr(pte2.ppn);
        for (int i1 = 0; i1 < PTE_CNT; i1++) {
            struct pte pte1 = pt1[i1];
            // skip invalid entries
            if (!PTE_VALID(pte1) || PTE_LEAF(pte1)) {
                continue;
            }
            // iterate thru all entries in the level 0 page table
            struct pte* pt0 = (struct pte*)pageptr(pte1.ppn);
            for (int i0 = 0; i0 < PTE_CNT; i0++) {
                struct pte pte0 = pt0[i0];
                // skip invalid entries
                if (!PTE_VALID(pte0) || !PTE_LEAF(pte0)) {
                    continue;
                }
                // free only non-global pages
                if (!PTE_GLOBAL(pte0)) {
                    void* pp = pageptr(pte0.ppn);
                    // free the page
                    free_phys_page(pp);
                    // unmap the page
                    pt0[i0] = null_pte();
                }
            }
        }
    }
    return; 
}

mtag_t discard_active_mspace(void) {
    // unmaps and frees all non-global pages from the previously active memory space
    reset_active_mspace();
    // Switches memory spaces to main
    switch_mspace(main_mtag);
    return main_mtag; 
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.


void * map_page(uintptr_t vma, void * pp, int rwxug_flags) {
    if (!wellformed(vma) || (vma % PAGE_SIZE) != 0 || pp == NULL) {
        return NULL;
    }
    // get the root page table
    struct pte* pt2 = active_space_ptab();
    // root -> level 1
    struct pte* pt1;
    // if the root page table entry is not valid, create a new level 1 page table
    if (!PTE_VALID(pt2[VPN2(vma)])) {
        pt1 = (struct pte*)alloc_phys_page();
        if (pt1 == NULL) {
            return NULL;
        }
        // zero out the page
        memset(pt1, 0, PAGE_SIZE);
        pt2[VPN2(vma)] = ptab_pte(pt1, 0);
    } else {
        pt1 = (struct pte*)pageptr(pt2[VPN2(vma)].ppn);
    }
    // level 1 -> level 0
    struct pte* pt0;
    // if the level 1 page table entry is not valid, create a new level 0 page table
    if (!PTE_VALID(pt1[VPN1(vma)])) {
        pt0 = (struct pte*)alloc_phys_page();
        if (pt0 == NULL) {
            return NULL;
        }
        // zero out the page
        memset(pt0, 0, PAGE_SIZE);
        pt1[VPN1(vma)] = ptab_pte(pt0, 0);
    } else {
        pt0 = (struct pte*)pageptr(pt1[VPN1(vma)].ppn);
    }

    if (PTE_VALID(pt0[VPN0(vma)])) {
        // if already mapped, do nothing
        // return (void *)vma;
        return NULL;
    }
    pt0[VPN0(vma)] = leaf_pte(pp, rwxug_flags);
    
    return (void *)vma;
}

void * map_range(uintptr_t vma, size_t size, void * pp, int rwxug_flags) {
    if (!wellformed(vma) || (vma % PAGE_SIZE) != 0 || pp == NULL || (size % PAGE_SIZE) != 0 || size == 0) {
        return NULL;
    }
    // number of pages to map
    size_t pages = size / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t virt_addr = vma + i * PAGE_SIZE;
        void * phys_addr = (void *)((uintptr_t)pp + i * PAGE_SIZE);
        // map the page
        if (!map_page(virt_addr, phys_addr, rwxug_flags)) {
            free_phys_page(phys_addr);
            unmap_and_free_range((void *)vma, i * PAGE_SIZE);
            return NULL;
        }
    }
    return (void *)vma; 
}

// void * alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
//     if (!wellformed(vma) || (vma % PAGE_SIZE) != 0 || (size % PAGE_SIZE) != 0 || size == 0) {
//         kprintf("[alloc_and_map_range] Invalid input: vma=%p size=%lx\n", (void *)vma, size);
//         return NULL;
//     }

//     size_t pages = size / PAGE_SIZE;
//     kprintf("[alloc_and_map_range] Mapping %lu pages starting at %p\n", pages, (void *)vma);

//     for (size_t i = 0; i < pages; i++) {
//         uintptr_t virt_addr = vma + i * PAGE_SIZE;
//         kprintf("[alloc_and_map_range] Page %lu: vaddr %p\n", i, (void *)virt_addr);

//         void * phys_addr = alloc_phys_page();
//         if (phys_addr == NULL) {
//             kprintf("[alloc_and_map_range] alloc_phys_page failed at page %lu\n", i);
//             unmap_and_free_range((void *)vma, i * PAGE_SIZE);
//             return NULL;
//         }

//         kprintf("[alloc_and_map_range] Got phys page %p\n", phys_addr);

//         if (!map_page(virt_addr, phys_addr, rwxug_flags)) {
//             kprintf("[alloc_and_map_range] map_page failed at vaddr %p\n", (void *)virt_addr);
//             free_phys_page(phys_addr);
//             unmap_and_free_range((void *)vma, i * PAGE_SIZE);
//             return NULL;
//         }

//         kprintf("[alloc_and_map_range] Mapped vaddr %p → phys %p with flags 0x%x\n",
//                 (void *)virt_addr, phys_addr, rwxug_flags);
//     }

//     kprintf("[alloc_and_map_range] DONE.\n");
//     return (void *)vma;
// }

void * alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    if (!wellformed(vma) || (vma % PAGE_SIZE) != 0 || (size % PAGE_SIZE) != 0 || size == 0) {
        return NULL;
    }
    // number of pages to map
    size_t pages = size / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t virt_addr = vma + i * PAGE_SIZE;
        void * phys_addr = alloc_phys_page();
        if (phys_addr == NULL) {
            unmap_and_free_range((void *)vma, i * PAGE_SIZE);
            return NULL;
        }
        // map the page
        if (!map_page(virt_addr, phys_addr, rwxug_flags)) {
            free_phys_page(phys_addr);
            unmap_and_free_range((void *)vma, i * PAGE_SIZE);
            return NULL;
        }
    }
    return (void *)vma; 
}

void set_range_flags(const void * vp, size_t size, int rwxug_flags) {
    if (!wellformed((uintptr_t)vp) || (uintptr_t)vp % PAGE_SIZE != 0 || (size % PAGE_SIZE) != 0 || size == 0) {
        return;
    }
    // number of pages to map
    size = ROUND_UP(size, PAGE_SIZE);
    size_t pages = size / PAGE_SIZE;

    uintptr_t virt_addr = (uintptr_t)vp;

    // get the root page table
    struct pte* pt2 = active_space_ptab();

    for (size_t i = 0; i < pages; i++) {
        uintptr_t curr_va = virt_addr + i * PAGE_SIZE;
        if (!wellformed(curr_va)) {
            continue;
        }
        struct pte* pt1, *pt0;
        // root -> level 1
        if (!PTE_VALID(pt2[VPN2(curr_va)])) {
            continue;
        } else {
            pt1 = (struct pte*)pageptr(pt2[VPN2(curr_va)].ppn);
        }
        // level 1 -> level 0
        if (!PTE_VALID(pt1[VPN1(curr_va)])) {
            continue;
        } else {
            pt0 = (struct pte*)pageptr(pt1[VPN1(curr_va)].ppn);
        }
        // leaf entry
        struct pte *leaf_pte = &pt0[VPN0(curr_va)];
        if (PTE_VALID(*leaf_pte) && PTE_LEAF(*leaf_pte)) {
            // set the flags
            leaf_pte->flags = rwxug_flags | PTE_A | PTE_D | PTE_V;
        } else {
            continue;
        }
    }
    return;
}

void unmap_and_free_range(void * vp, size_t size) {
    if (!wellformed((uintptr_t)vp) || (uintptr_t)vp % PAGE_SIZE != 0 || (size % PAGE_SIZE) != 0 || size == 0) {
        return;
    }
    size = ROUND_UP(size, PAGE_SIZE);
    size_t pages = size / PAGE_SIZE;
    uintptr_t virt_addr = (uintptr_t)vp;
    // get the root page table
    struct pte* pt2 = active_space_ptab();
    for (size_t i = 0; i < pages; i++) {
        uintptr_t curr_va = virt_addr + i * PAGE_SIZE;
        if (!wellformed(curr_va)) {
            continue;
        }
        struct pte* pt1, *pt0;
        if (!PTE_VALID(pt2[VPN2(curr_va)])) {
            continue;
        } else {
            pt1 = (struct pte*)pageptr(pt2[VPN2(curr_va)].ppn);
        }
        // level 1 -> level 0
        if (!PTE_VALID(pt1[VPN1(curr_va)])) {
            continue;
        } else {
            pt0 = (struct pte*)pageptr(pt1[VPN1(curr_va)].ppn);
        }
        struct pte *leaf_pte = &pt0[VPN0(curr_va)];
        if (PTE_VALID(*leaf_pte) && PTE_LEAF(*leaf_pte)) {
            void *pp = pageptr(leaf_pte->ppn); // convert ppn => address
            free_phys_page(pp); // free the page
            *leaf_pte = null_pte(); // unmap the page
        }
    }
    return;
}

void * alloc_phys_page(void) {
    // return the address of the allocated page
    return alloc_phys_pages(1);
}

void free_phys_page(void * pp) {
    free_phys_pages(pp, 1);
    return; 
}

void * alloc_phys_pages(unsigned int cnt) {
    if (cnt ==0 || cnt > free_phys_page_count()) {
        return NULL;
    }
    struct page_chunk **curr_ptr = &free_chunk_list;
    struct page_chunk *best_chunk = NULL;
    // for the new head
    struct page_chunk **best_ptr = NULL;
    // find the smallest chunk
    while (*curr_ptr) {
        // local copy
        struct page_chunk *curr = *curr_ptr;
        // check if the chunk is big enough
        if (curr->pagecnt >= cnt) {
            if (!best_chunk || curr->pagecnt < best_chunk->pagecnt) {
                best_chunk = curr;
                best_ptr = curr_ptr;
            }
        }
        curr_ptr = &curr->next;
    }
    if (!best_chunk) {
        panic("no chunk found");
    }
    if (best_chunk->pagecnt == cnt) {
        *best_ptr = best_chunk->next;
    } else {
        // Create a new chunk in the remaining space after allocation
        struct page_chunk *new_chunk = (struct page_chunk *)((uintptr_t)best_chunk + cnt * PAGE_SIZE);
        new_chunk->next = best_chunk->next;
        new_chunk->pagecnt = best_chunk->pagecnt - cnt;
        *best_ptr = new_chunk;
    }
    void *result = (void *)best_chunk;
    return result;
}

void free_phys_pages(void * pp, unsigned int cnt) {
    if (pp == NULL || cnt == 0) {
        return;
    }
    struct page_chunk * chunk = (struct page_chunk*)pp;
    // add the chunk to the free chunk list
    chunk->next = free_chunk_list;
    chunk->pagecnt = cnt;
    // update the free chunk list
    free_chunk_list = chunk;
    return; 
}

unsigned long free_phys_page_count(void) {
    unsigned long cnt = 0;
    struct page_chunk * curr = free_chunk_list;
    while (curr) {
        cnt += curr->pagecnt;
        curr = curr->next;
    }
    return cnt;
}

int handle_umode_page_fault(struct trap_frame * tfr, uintptr_t vma) {
    if (!wellformed(vma) || (vma % PAGE_SIZE) != 0 || vma < UMEM_START_VMA || vma >= UMEM_END_VMA) {
        return 0; // no handled
    }
    void *pp = alloc_phys_page();
    if (pp == NULL) {
        return 0; // no handled
    }
    if (!map_page(vma, pp, PTE_R | PTE_W | PTE_U)) {
        free_phys_page(pp);
        return 0; // no handled
    }
    return 1;
}


mtag_t active_space_mtag(void) {
    return csrr_satp();
}

static inline mtag_t ptab_to_mtag(struct pte * ptab, unsigned int asid) {
    return (
        ((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
        ((unsigned long)asid << RISCV_SATP_ASID_shift) |
        pagenum(ptab) << RISCV_SATP_PPN_shift);
}

static inline struct pte * mtag_to_ptab(mtag_t mtag) {
    return (struct pte *)((mtag << 20) >> 8);
}

static inline struct pte * active_space_ptab(void) {
    return mtag_to_ptab(active_space_mtag());
}

static inline void * pageptr(uintptr_t n) {
    return (void*)(n << PAGE_ORDER);
}

static inline unsigned long pagenum(const void * p) {
    return (unsigned long)p >> PAGE_ORDER;
}

static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits+1));
}

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags) {
    return (struct pte) {
        .flags = rwxug_flags | PTE_A | PTE_D | PTE_V,
        .ppn = pagenum(pp)
    };
}

static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag) {
    return (struct pte) {
        .flags = g_flag | PTE_V,
        .ppn = pagenum(pt)
    };
}


static inline struct pte null_pte(void) {
    return (struct pte) { };
}

int validate_vptr(const void *vp, size_t len, uint_fast8_t rwxug_flags) {
    if (vp == NULL || len == 0) {
        return -EINVAL;
    }
    uintptr_t virt_addr = (uintptr_t)vp;
    uintptr_t end_addr = virt_addr + len;
    if (!wellformed(virt_addr) || !wellformed(end_addr - 1)) {
        return -EINVAL;
    }
    while (virt_addr < end_addr) {
        // get the root page table
        struct pte* pt2 = active_space_ptab();
        if (!PTE_VALID(pt2[VPN2(virt_addr)])) {
            return -EINVAL;
        }
        struct pte* pt1 = (struct pte*)pageptr(pt2[VPN2(virt_addr)].ppn);
        if (!PTE_VALID(pt1[VPN1(virt_addr)])) {
            return -EINVAL;
        }
        struct pte* pt0 = (struct pte*)pageptr(pt1[VPN1(virt_addr)].ppn);
        struct pte *leaf_pte = &pt0[VPN0(virt_addr)];
        if (!PTE_VALID(*leaf_pte) || !PTE_LEAF(*leaf_pte)) {
            return -EINVAL;
        }
        if ((leaf_pte->flags & rwxug_flags) != rwxug_flags) {
            return -EACCESS;
        }
        // Advance to next page or stop at end
        uintptr_t next_page = ROUND_UP(virt_addr + 1, PAGE_SIZE);
        virt_addr = (next_page < end_addr) ? next_page : end_addr;
    }
    return 0;
}

int validate_vstr(const char *vs, uint_fast8_t ug_flags) {
    if (vs == NULL || !wellformed((uintptr_t)vs)) {
        return -EINVAL;
    }
    const char *ptr = vs;
    size_t max_len = 8192;
    for (size_t i = 0; i < max_len; i++) {
        if (*ptr == '\0') {
            return 0;
        }
        if (validate_vptr(ptr, 1, ug_flags) != 0) {
            return -EACCESS;
        }
        ptr++;
    }
    return -EINVAL;
}