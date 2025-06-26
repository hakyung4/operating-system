// heap.h - Heap memory allocator
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _HEAP_H_
#define _HEAP_H_

#include <stddef.h>

#ifndef HEAP_ALLOC_MAX
#define HEAP_ALLOC_MAX 4000
#endif

extern char heap_initialized;
extern void heap_init(void * start, void * end);

extern void * kmalloc(size_t size);
extern void * kcalloc(size_t nelts, size_t eltsz);
extern void kfree(void * ptr);

#endif // _HEAP_H_