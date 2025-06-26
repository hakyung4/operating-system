// cache.h - Block cache for a storage device
//
// Copyright (c) 2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _CACHE_H_
#define _CACHE_H_

#define CACHE_BLKSZ 512UL 
#define CAPACITY 64UL

#define CACHE_CLEAN 0
#define CACHE_DIRTY 1

struct io; // extern decl.
struct cache; // opaque decl.

extern int create_cache(struct io * bkgio, struct cache ** cptr);
extern int cache_get_block(struct cache * cache, unsigned long long pos, void ** pptr);
extern void cache_release_block(struct cache * cache, void * pblk, int dirty);
extern int cache_flush(struct cache * cache);

#endif // _CACHE_H_