#include "cache.h"
#include "io.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "cache.h"
#include "ioimpl.h"
#include "thread.h"



struct cache_entry {
    unsigned long long pos; // position of block in device
    void *block;
    int valid;
    int dirty;
    struct cache_entry * next;
};


struct cache {
    struct io* bkgio;
    struct cache_entry * head;
    struct cache_entry * tail;
    struct lock cache_lock;
};

int create_cache(struct io *bkgio, struct cache **cptr) {
    if (!bkgio || !cptr) {
        return -EINVAL;
    }

    struct cache *cache = kmalloc(sizeof(struct cache));
    if (!cache) {
        return -ENOMEM;
    }

    cache->bkgio = bkgio;
        
    lock_init(&cache->cache_lock);
    cache->head = NULL;
    cache->tail = NULL;

    // Build a linked list of CACHE_CAPACITY entries
    for (int i = 0; i < CAPACITY; i++) {
        struct cache_entry *node = kmalloc(sizeof(struct cache_entry));
        if (!node) {
            // partial failure => free if you want
            return -ENOMEM;
        }

        node->block = kmalloc(CACHE_BLKSZ);
        if (!node->block) {
            // partial failure => free
            return -ENOMEM;
        }

        node->pos   = 0ULL;
        node->valid = 0;
        node->dirty = 0;
        node->next  = NULL;

        if (cache->head == NULL) {
            // first node
            cache->head = node;
            cache->tail = node;
        } else {
            cache->tail->next = node;
            cache->tail = node;
        }
    }

    *cptr = cache;
    return 0;
}


int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {

    // check first if block is in cache. if it is, get the block from cache
    // if not, read in from the device
    // ● Pointer to block data (512 bytes) returned via pptr argument
    // non zero error on failure
    // caller has exclusive use of block -> need to think about how to handle this -> walk through seems easier
    // block remain in memory until released using cahce_release_block
    // must call cache_release_block to release the block
    // for eviction, use LRU

    if (cache == NULL || pptr == NULL) {
        return -EINVAL;
    }
    
    lock_acquire(&cache->cache_lock);


    struct cache_entry * curr = cache->head;
    struct cache_entry * prev = NULL;

    // check if the block is already in the cache
    while(curr != NULL) {
        if (curr->valid && curr->pos == pos) {
            *pptr = curr->block;
            if(prev != NULL){ // since we got a hit move this node to the front of the linked list
                prev->next = curr->next;
                if(curr == cache->tail){
                    cache->tail = prev;
                }
                curr->next = cache->head;
                cache->head = curr;
            }
            lock_release(&cache->cache_lock);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    curr = cache->head;
    prev = NULL;

    // if not, find an empty entry in the cache
    while(curr != NULL) {
        if (!curr->valid) {
            curr->valid = 1;
            curr->pos = pos;
            *pptr = curr->block;

            // read the block from the device
            long rcnt = ioreadat(cache->bkgio, pos, curr->block, CACHE_BLKSZ);
            if (rcnt < 0) {
                curr->valid = 0;
                lock_release(&cache->cache_lock);
                return rcnt;
            }
            lock_release(&cache->cache_lock);
            return 0;
        }
        curr = curr->next;
    }


    if (cache->tail->dirty == 1) {
        iowriteat(cache->bkgio, cache->tail->pos,
                cache->tail->block, CACHE_BLKSZ);
        cache->tail->dirty = 0;
    }

    //evict the oldest one

    cache->tail->pos = pos;
    cache->tail->valid = 1;
    *pptr = cache->tail->block;

    long rcnt = ioreadat(cache->bkgio, pos, cache->tail->block, CACHE_BLKSZ);
    if (rcnt < 0) {
        cache->tail->valid = 0;
        lock_release(&cache->cache_lock);
        return rcnt;
    }

    if(cache->head == cache->tail){
        lock_release(&cache->cache_lock);
        return 0;
    }

    curr = cache->head;
    while(curr->next != cache->tail){
        curr = curr->next;
    }
    curr->next = NULL;
    if(cache->head != cache->tail){
        cache->tail->next = cache->head;
    }
    cache->head = cache->tail;
    cache->tail = curr;
    lock_release(&cache->cache_lock);
    return 0;
}

//pblk is a pointer to a block that was made available in cache_get_block() (which means that pblk == *pptr 
//for some pptr). If dirty==1, the block has been written to. If dirty==0, the block has not been written to.

extern void cache_release_block(struct cache * cache, void * pblk, int dirty){

    if(cache == NULL || pblk == NULL){
        return;
    }

    lock_acquire(&cache->cache_lock);

    struct cache_entry * curr = cache->head;

    while(curr != NULL){
        if(curr->valid && curr->block == pblk){
            if(dirty){
                iowriteat(cache->bkgio, curr->pos, curr->block, CACHE_BLKSZ);
                curr->dirty = 0;
            }
            lock_release(&cache->cache_lock);
            return;
        }
        curr = curr->next;
    }


    lock_release(&cache->cache_lock);
    return;
}

//This function flushes the cache. Any dirty blocks that have not yet been written to the backing interface 
//must be written to the backing interface. Returns 0 if successful.

extern int cache_flush(struct cache * cache){
    if(cache == NULL){
        return -EINVAL;
    }

    lock_acquire(&cache->cache_lock);

    struct cache_entry * curr = cache->head;

    while(curr != NULL){
            if(curr->dirty && curr->valid){
                iowriteat(cache->bkgio, curr->pos, curr->block, CACHE_BLKSZ);
                curr->dirty = 0;
            }
            curr = curr->next;
        }

    lock_release(&cache->cache_lock);
    return 0;

}

    