// heap.h - User heap memory allocator
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#include <stddef.h>

extern char heap_initialized;

extern void heap_init(void * start, void * end);
extern void * malloc(size_t size);
extern void * calloc(size_t nelts, size_t eltsz);
extern void free(void * ptr);
