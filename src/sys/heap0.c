// heap0.c - Simple, heap memory manager
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//



#ifdef HEAP_TRACE
#define TRACE
#endif

#ifdef HEAP_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "assert.h"
#include "memory.h"

#include <stddef.h>
#include <stdint.h>

#ifndef HEAP_ALIGN

#define HEAP_ALIGN 16
#endif


#define HEAP_ALLOC_MAGIC 0xEAEAEAEA


#define HEAP_FREE_MAGIC 0x25252525

// INTERNAL TYPE DEFINITIONS
//

//        +----------------+----------------+
//        |  ALLOC_MAGIC   |      size      |
//        +----------------+----------------+
//        |   size_inv     |    alloc_ra    |
// ptr -> +----------------+----------------+
//        |  FREE_MAGIC    |    free_ra     |
//        +---------------------------------+
//


// Header that preceeds each allocated block. Must be a multiple of HEAP_ALIGN.

struct heap_alloc_header {
    uint32_t magic; ///< HEAP_ALLOC_MAGIC
    uint32_t size; ///< Size of memory block being allocated
    uint32_t size_inv; ///< Bitwise not of the size
    uint32_t ra32;    ///< Caller return address
};


struct heap_free_record {
    uint32_t magic; ///< HEAP_FREE_MAGIC
    uint32_t ra32; ///< Caller return address
};

// The ISPOW2 macro evaluates to 1 if its argument is either zero or a power of
// two. The argument must be an integer type. Cast pointers to uintptr_t to test
// pointer alignment.


#define ISPOW2(n) (((n)&((n)-1)) == 0)

// INTERNAL GLOBAL VARIABLES
//


static void * heap_low; // lowest address of heap memory


static void * heap_end; // end of heap memory


// INTERNAL FUNCTION DEFINITIONS
//

static void * heap_malloc_actual(size_t size, void * ra);
static void * heap_calloc_actual(size_t nelts, size_t eltsz, void * ra);
static void heap_free_actual(void * ptr, void * ra);

// EXPORTED GLOBAL VARIABLES
//

char heap_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void heap_init(void * start, void * end) {
    trace("%s(%p,%p)", __func__, start, end);

    assert (4 <= HEAP_ALIGN);
    assert (ISPOW2(HEAP_ALIGN));

    // Round start up and end down to a HEAP_ALIGN boundary

    start = (void*)ROUND_UP((uintptr_t)start, HEAP_ALIGN);
    end = (void*)ROUND_DOWN((uintptr_t)end, HEAP_ALIGN);
    assert (start < end);

    heap_low = start;
    heap_end = end;
    heap_initialized = 1;
}

void * kmalloc(size_t size) {
    return heap_malloc_actual(size, __builtin_return_address(0));
}

void * kcalloc(size_t nelts, size_t eltsz) {
    return heap_calloc_actual(nelts, eltsz, __builtin_return_address(0));
}

void kfree(void * ptr) {
    return heap_free_actual(ptr, __builtin_return_address(0));
}

// INTERNAL FUNCTION DEFINITIONS
//


void * heap_malloc_actual(size_t size, void * ra) {
    struct heap_alloc_header * hdr;
    size_t leftover;
    void * newpage;
    void * ptr;

    trace("%s(%zu,ra=%p)", __func__, size, ra);

    if (size == 0)
        return NULL;

    size = ROUND_UP(size, HEAP_ALIGN);

    if (HEAP_ALLOC_MAX < size)
        panic("malloc request too large");
    
    // Check if request is larger than remaining heap (include overflow). If we
    // implement heap growth (HAVE_MEMORY is defined), ask for another page from
    // the page allocator.

    if (size + sizeof(struct heap_alloc_header) <= heap_end - heap_low) {
        // have enough in current pool
        ptr = heap_end - size;
        heap_end = (struct heap_alloc_header*)ptr - 1;
    } else {
        // need to get another page
        if (PAGE_SIZE - sizeof(struct heap_alloc_header) < size)
            panic(NULL);
        
        // Decide whether to switch to the new page or satisfy allocation
        // request from new page but keep using old heap. Here, _leftover_ is
        // the space left in the page after we satisfy the allocation request.

        newpage = alloc_phys_page();
        ptr = newpage + PAGE_SIZE - size;
        leftover = PAGE_SIZE - size - sizeof(struct heap_alloc_header);

        if (heap_end - heap_low < leftover) {
            heap_end = ptr - sizeof(struct heap_alloc_header);
            heap_low = newpage;
        }
    }

    hdr = (struct heap_alloc_header*)ptr - 1;
    hdr->magic = HEAP_ALLOC_MAGIC;
    hdr->size = size;
    hdr->size_inv = ~size;
    hdr->ra32 = (uint32_t)(uintptr_t)ra;

    memset(ptr, 0x33, size);
    return ptr;
}


void * heap_calloc_actual(size_t nelts, size_t eltsz, void * ra) {
    size_t size;
    void * ptr;

    trace("%s(%zu,%zu,ra=%p)", __func__, nelts, eltsz, ra);

    assert (nelts <= HEAP_ALLOC_MAX / eltsz);
    size = nelts * eltsz;

    ptr = heap_malloc_actual(size, ra);
    memset(ptr, 0, size);
    return ptr;
}


void heap_free_actual(void * ptr, void * ra) {
    struct heap_alloc_header * hdr;
    struct heap_free_record * rec;

    trace("%s(%p,ra=%p)", __func__, ptr, ra);

    // Make pointers to alloc header and free record

    hdr = ptr;
    hdr -= 1;
    rec = ptr;

    // Check integrity

    if (hdr->size != ~hdr->size_inv) {
        if (hdr->magic != HEAP_ALLOC_MAGIC)
            panic(NULL);
        else if (hdr->size_inv == 0 && rec->magic == HEAP_FREE_MAGIC)
            panic(NULL);
        else
            panic(NULL);
    }
    
    memset(rec+1, 0x11, hdr->size - sizeof(struct heap_free_record));
    rec->magic = HEAP_FREE_MAGIC;
    rec->ra32 = (uint32_t)(uintptr_t)ra;
    hdr->size_inv = 0;
}