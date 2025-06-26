// ktfs.c - KTFS implementation
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#define MAX_OPEN_FILES 96UL


#include "heap.h"
#include "fs.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
#include "cache.h"

// INTERNAL TYPE DEFINITIONS
//


static struct cache * file_system_cache;


struct master_ktfs {
    struct io io; // you can keep or remove
    struct ktfs_superblock superblock;
    uint32_t bitmap_start_block; // block index in disk for the first bitmap block
    uint32_t inode_start_block;  // block index for the first inode block
    uint32_t data_start_block;   // block index for first data block
    struct lock ktfs_lock;
};

static struct master_ktfs * ktfs_master; 



struct ktfs_file {
    // Fill to fulfill spec
    struct io io;
    struct ktfs_dir_entry ktfs_dir_entry;
    int in_use; // flag indicating if the file is currently open
    unsigned long long fsize;
    int flags; // file mode flags (write/read mode)
    unsigned long long offset; // current file offset for sequential reads/writes
};

static struct ktfs_file open_files[MAX_OPEN_FILES];



// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_mount(struct io * io);

int ktfs_open(const char * name, struct io ** ioptr);
void ktfs_close(struct io* io);
long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len);
int ktfs_cntl(struct io *io, int cmd, void *arg);

int ktfs_getblksz(struct ktfs_file *fd);
int ktfs_getend(struct ktfs_file *fd, void *arg);

int ktfs_flush(void);

int get_inode(uint16_t inode_num, struct ktfs_inode * output_inode, int delete);

int put_inode(uint16_t inode_num, const struct ktfs_inode *input_inode);

int find_inode_by_name(const char * name, uint16_t * inode_num, int delete);

int ktfs_create	(const char * name);

int ktfs_delete (const char * name);

long ktfs_writeat(struct io* io, unsigned long long pos, const void * buf, long len);


static const struct iointf ktfs_iointf = {
    .close = &ktfs_close,
    .readat = &ktfs_readat,
    .cntl = &ktfs_cntl,
    .writeat = &ktfs_writeat
};

// FUNCTION ALIASES
//

int fsmount(struct io * io)
    __attribute__ ((alias("ktfs_mount")));

int fsopen(const char * name, struct io ** ioptr)
    __attribute__ ((alias("ktfs_open")));

int fsflush(void)
    __attribute__ ((alias("ktfs_flush")));

int fscreate(const char * name)
    __attribute__ ((alias("ktfs_create")));

int fsdelete(const char * name)
    __attribute__ ((alias("ktfs_delete")));

// EXPORTED FUNCTION DEFINITIONS
//


int find_inode_by_name(const char *name, uint16_t *inode_num, int delete)
{
    if (!name || !inode_num)
        return -EINVAL;

    /* ------------ 1. fetch root inode ---------------------------------- */
    uint16_t root_ino = ktfs_master->superblock.root_directory_inode;
    struct ktfs_inode root;
    int ret = get_inode(root_ino, &root, 0);
    if (ret < 0) return ret;

    const int dentries_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    int total = root.size / sizeof(struct ktfs_dir_entry);

    int global_idx = 0;
    // uint32_t found_phys = 0;
    int      found_slot = -1;
    struct ktfs_dir_entry *found_blk = NULL;

    /* ------------ 2. scan direct blocks for NAME ----------------------- */
    for (int bi = 0; bi < KTFS_NUM_DIRECT_DATA_BLOCKS && global_idx < total; ++bi) {
        uint32_t blkno = root.block[bi];

        uint32_t phys = (blkno + ktfs_master->data_start_block) * KTFS_BLKSZ;
        struct ktfs_dir_entry *blk_ptr;
        ret = cache_get_block(file_system_cache, phys, (void**)&blk_ptr);
        if (ret < 0) return ret;

        for (int ei = 0; ei < dentries_per_blk && global_idx < total; ++ei, ++global_idx) {
            if (strcmp(name, blk_ptr[ei].name) == 0) {
                *inode_num  = blk_ptr[ei].inode;
                // found_phys  = phys;
                found_slot  = ei;
                found_blk   = blk_ptr;           /* keep pinned */
                goto after_scan;
            }
        }
        cache_release_block(file_system_cache, blk_ptr, 0);
    }
    return -ENOENT;                               /* not found */

after_scan:

    /* ------------ 3. lookup only? ------------------------------------- */
    if (!delete) {
        cache_release_block(file_system_cache, found_blk, 0);
        return 0;
    }

    /* ------------ 4. identify last dentry ----------------------------- */
    int last_idx   = total - 1;                   /* index BEFORE shrink      */
    int last_blk_i = last_idx / dentries_per_blk;
    int last_slot  = last_idx % dentries_per_blk;
    uint32_t last_blkno = root.block[last_blk_i];

    struct ktfs_dir_entry *last_blk;
    ret = cache_get_block(file_system_cache, (last_blkno + ktfs_master->data_start_block) * KTFS_BLKSZ, (void**)&last_blk);
    if (ret < 0) { cache_release_block(file_system_cache, found_blk, 0); return ret; }

    /* ------------ 5. swap‑with‑last (if needed) ----------------------- */
    if (last_idx != global_idx)
        found_blk[found_slot] = last_blk[last_slot];

    memset(&last_blk[last_slot], 0, sizeof(struct ktfs_dir_entry));

    /* mark dirty + release (avoid double free) */
    cache_release_block(file_system_cache, found_blk, 1);
    if (last_blk != found_blk)
        cache_release_block(file_system_cache, last_blk, 1);

    /* ------------ 6. shrink root.dir size ----------------------------- */
    root.size -= sizeof(struct ktfs_dir_entry);

    /* ------------ 7. if last block became empty, free it -------------- */
    // if (root.size % KTFS_BLKSZ == 0) {
    //     int freed_idx = root.size / KTFS_BLKSZ;       /* new #data blocks   */
    //     if (freed_idx < KTFS_NUM_DIRECT_DATA_BLOCKS && root.block[freed_idx]) {
    //         uint32_t blkno = root.block[freed_idx];   /* data‑region index */
    //         /* clear bitmap bit ------------------------------------------------ */
    //         uint32_t bmap_block   = blkno / (KTFS_BLKSZ * 8);
    //         int      bit_in_block = blkno % (KTFS_BLKSZ * 8);
    //         int      byte_idx     = bit_in_block / 8;
    //         int      bit_ofs      = bit_in_block % 8;

    //         uint8_t *bmp_ptr;
    //         ret = cache_get_block(file_system_cache,
    //                 (ktfs_master->bitmap_start_block + bmap_block) * KTFS_BLKSZ,
    //                 (void**)&bmp_ptr);
    //         if (ret < 0) return ret;

    //         bmp_ptr[byte_idx] &= ~(1 << bit_ofs);     /* clear bit */
    //         cache_release_block(file_system_cache, bmp_ptr, 1);

    //         root.block[freed_idx] = 0;                 /* clear ptr */
    //     }
    // }

    /* ------------ 8. persist root inode ------------------------------- */
    return put_inode(root_ino, &root);
}

int get_inode(uint16_t inode_num, struct ktfs_inode * output_inode, int delete){

    if (!output_inode) {
        return -EINVAL;
    }

    int inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ;

    int inode_block_idx = inode_num / inodes_per_block;

    int offset_in_block = inode_num % inodes_per_block;

    uint32_t inode_start_region = ktfs_master->inode_start_block;

    uint32_t block_num = inode_start_region + inode_block_idx;

    struct ktfs_inode * inode_ref;

    int ret = cache_get_block(file_system_cache, block_num*KTFS_BLKSZ, (void**)&inode_ref);

    if(ret < 0){
        return ret;
    }

    struct ktfs_inode *on_disk_inode = &inode_ref[offset_in_block];

    memcpy(output_inode, on_disk_inode, sizeof(struct ktfs_inode));

    if(delete){
        memset(on_disk_inode, 0, sizeof(struct ktfs_inode));
    }

    cache_release_block(file_system_cache, inode_ref, delete);

    return 0;

}

int put_inode(uint16_t inode_num, const struct ktfs_inode *input_inode)
{
    if (!input_inode) {
        return -EINVAL;
    }

    int inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ;
    int inode_block_idx = inode_num / inodes_per_block;
    int offset_in_block = inode_num % inodes_per_block;

    // Where the inode region starts
    // (This depends on how your disk layout is set up.
    //  Typically: [superblock = block 0], [bitmap blocks], then [inode blocks].)
    uint32_t inode_start_region = ktfs_master->inode_start_block;

    // This is the block index of the block containing the desired inode
    uint32_t block_num = inode_start_region + inode_block_idx;

    // Pull that block into memory
    struct ktfs_inode *inode_ref;
    int ret = cache_get_block(file_system_cache, block_num * KTFS_BLKSZ, (void**)&inode_ref);
    if (ret < 0) {
        return ret; 
    }

    // The specific inode in that block
    struct ktfs_inode *on_disk_inode = &inode_ref[offset_in_block];

    // Copy our in-memory inode data out to disk
    memcpy(on_disk_inode, input_inode, sizeof(struct ktfs_inode));

    // Release the block, marking it dirty so it is written back
    cache_release_block(file_system_cache, inode_ref, 1);

    return 0;
}



int ktfs_mount(struct io * io)
{
    if (io == NULL) {
        return -EINVAL;
    }

    // ioinit1(io, &ktfs_iointf);

    int result = create_cache(io, &file_system_cache);
    // return the error code if applicable
    if (result < 0) {
        return result;
    }

    ktfs_master = kcalloc(1, sizeof(struct master_ktfs));

    if (!ktfs_master) {
        return -ENOMEM;
    }

    ktfs_master->io = *io;

    struct ktfs_superblock *sbptr = NULL;
    result = cache_get_block(file_system_cache, 0ULL, (void**)&sbptr);
    if (result < 0) {
        //kfree(ktfs_master);
        return result;
    }

    memcpy(&ktfs_master->superblock, sbptr, sizeof(struct ktfs_superblock));

    cache_release_block(file_system_cache, sbptr, 0);

    // uint32_t block_count = ktfs_master->superblock.block_count;
    uint32_t B = ktfs_master->superblock.bitmap_block_count;
    uint32_t N = ktfs_master->superblock.inode_block_count;
    // uint16_t root_ino = ktfs_master->superblock.root_directory_inode;

    // calculate start blocks for each region
    //     superblock = block #0
    //     bitmaps = block #1..(1+B-1)
    //     inodes = block #(1+B)..(1+B+N-1)
    //     data   = block #(1+B+N)..end
    ktfs_master->bitmap_start_block = 1;
    ktfs_master->inode_start_block = 1 + B;
    ktfs_master->data_start_block = 1 + B + N;

    lock_init(&ktfs_master->ktfs_lock);

    return 0;
}

int ktfs_open(const char * name, struct io ** ioptr)
{
    if (name == NULL || ioptr == NULL) {
        return -EINVAL;
    }

    lock_acquire(&ktfs_master->ktfs_lock);


    for(int i = 0; i < MAX_OPEN_FILES; i++){
        if(open_files[i].in_use == 1){
            if(strcmp(open_files[i].ktfs_dir_entry.name, name) == 0){
                lock_release(&ktfs_master->ktfs_lock);
                return -EBUSY;
            }
        }
    }

    int empty_index = -1;

    for(int i = 0; i < MAX_OPEN_FILES; i++){
        if(open_files[i].in_use == 0){
            empty_index = i;
            break;
        }
    }

    if(empty_index < 0){
        lock_release(&ktfs_master->ktfs_lock);
        return -EMFILE;
    }

    int ret;
    uint16_t inode_num;

    ret = find_inode_by_name(name, &inode_num, 0);

    if(ret < 0){
        lock_release(&ktfs_master->ktfs_lock);
        return ret;
    }

    struct ktfs_inode inode;


    ret = get_inode(inode_num, &inode, 0);

    
    if(ret < 0){
        lock_release(&ktfs_master->ktfs_lock);
        return ret;
    }


    struct ktfs_file * file_to_open = &open_files[empty_index];

    memset(file_to_open, 0, sizeof(struct ktfs_file));

    file_to_open->in_use = 1;

    strncpy(file_to_open->ktfs_dir_entry.name, name, KTFS_MAX_FILENAME_LEN);

    file_to_open->ktfs_dir_entry.inode = inode_num;

    file_to_open->offset = 0ULL;

    file_to_open->fsize = inode.size;

    file_to_open->flags = inode.flags;

    ioinit1(&file_to_open->io, &ktfs_iointf);

    *ioptr = create_seekable_io(&file_to_open->io);

    lock_release(&ktfs_master->ktfs_lock);

    
    return 0;
}

void ktfs_close(struct io* io)
{
    if(io == NULL){
        return;
    }
    
    struct ktfs_file * const file = (void*)io - offsetof(struct ktfs_file, io);

    file->in_use = 0;

    return;
}

int ktfs_get_data_block(struct ktfs_inode * inode, int file_block_index){
    if (file_block_index < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        return inode->block[file_block_index];
    }
    if(file_block_index < 128 + KTFS_NUM_DIRECT_DATA_BLOCKS){
        uint32_t *ind_array;
        int ret = cache_get_block(file_system_cache, (inode->indirect + ktfs_master->data_start_block) * KTFS_BLKSZ, (void **)&ind_array);
        if (ret < 0) {
            // handle error: maybe return 0 or store the error in a global?
            return -1;
        }

        // find the pointer
        uint32_t blocknum = ind_array[file_block_index - KTFS_NUM_DIRECT_DATA_BLOCKS];

        cache_release_block(file_system_cache, ind_array, 0);

        return blocknum;
    }


    // Each double-indirect covers 128*128 blocks
    int blocks_in_dind = 128 * 128;

    if (file_block_index < blocks_in_dind + 131) { // there are 128 indirect blocks and 3 direct blocks
        uint32_t dind_block_num = inode->dindirect[0];
        if (dind_block_num == 0) {
            // not allocated => read is out of range => return 0
            return -1;
        }

        int index_in_dind = file_block_index - (KTFS_NUM_DIRECT_DATA_BLOCKS + 128);

        int top_index = index_in_dind / 128;

        int bottom_index = index_in_dind % 128;

        // read the double-indirect block
        uint32_t *dind_array;
        int ret = cache_get_block(file_system_cache, (dind_block_num + ktfs_master->data_start_block) * KTFS_BLKSZ,
                                    (void **)&dind_array);
        if (ret < 0) {
            // handle error
            return -1;
        }

        uint32_t second_level_block = dind_array[top_index];
        cache_release_block(file_system_cache, dind_array, 0);

        if (second_level_block == 0) {
            return -1;  // not allocated
        }

        uint32_t *second_level_array;
        ret = cache_get_block(file_system_cache, (second_level_block + ktfs_master->data_start_block) * KTFS_BLKSZ, (void **)&second_level_array);
        if (ret < 0) {
            return -1;
        }

        uint32_t real_block = second_level_array[bottom_index];
        cache_release_block(file_system_cache, second_level_array, 0);

        return real_block;
    } 
    else if(file_block_index < 2*blocks_in_dind + 131){
        uint32_t dind_block_num = inode->dindirect[1];
        if (dind_block_num == 0) {
            // not allocated => read is out of range => return 0
            return -1;
        }

        int index_in_dind = file_block_index - (131 + 128*128);

        int top_index = index_in_dind / 128;
        int bottom_index = index_in_dind % 128;

        uint32_t *dind_array;
        int ret = cache_get_block(file_system_cache, (dind_block_num + ktfs_master->data_start_block) * KTFS_BLKSZ,
                                    (void **)&dind_array);
        if (ret < 0) {
            // handle error
            return -1;
        }

        uint32_t second_level_block = dind_array[top_index];
        cache_release_block(file_system_cache, dind_array, 0);

        if (second_level_block == 0) {
            return -1;  // not allocated
        }

        uint32_t *second_level_array;
        ret = cache_get_block(file_system_cache, (second_level_block + ktfs_master->data_start_block) * KTFS_BLKSZ, (void **)&second_level_array);
        if (ret < 0) {
            return -1;
        }

        uint32_t real_block = second_level_array[bottom_index];
        cache_release_block(file_system_cache, second_level_array, 0);


        return real_block;
    }

    // // If we get here we are out of range
    return -1;
}

long ktfs_writeat (struct io * io, unsigned long long pos, const void * buf, long len)
{
    struct ktfs_file * file = (void*)io - offsetof(struct ktfs_file, io);

    if (pos >= file->fsize)
        return 0;
    if (len < 0)
        return -EINVAL;

    if (pos + len > file->fsize) {
        len = file->fsize - pos;
    }

    struct ktfs_inode file_inode;

    lock_acquire(&ktfs_master->ktfs_lock);
    
    int ret = get_inode(file->ktfs_dir_entry.inode, &file_inode, 0);

    if(ret < 0){
        lock_release(&ktfs_master->ktfs_lock);
        return ret;
    }

    int block_index = 0;

    int block_offset = 0;

    long total_written = 0;

    while(total_written < len){
        unsigned long long curr_offset = pos + total_written;

        block_index = curr_offset / 512;

        block_offset = curr_offset % 512;

        long remaining = len - total_written;

        long chunk = 512 - block_offset;

        if(chunk > remaining){
            chunk = remaining;
        }

        uint32_t curr_block_num = ktfs_get_data_block(&file_inode, block_index);

        if (curr_block_num == (uint32_t)-1) {
            // block is not allocated or out of range
            if (total_written > 0) {
                lock_release(&ktfs_master->ktfs_lock);
                return total_written;  // partial write success
            } else {
                lock_release(&ktfs_master->ktfs_lock);
                return -EIO;           // or some error code if no bytes were written yet
            }
        }

        void *blk_ptr;
        ret = cache_get_block(file_system_cache, (curr_block_num + ktfs_master->data_start_block) * KTFS_BLKSZ, (void**)&blk_ptr);
        if (ret < 0) {
            // partial read so far is total_read
            lock_release(&ktfs_master->ktfs_lock);
            return (total_written > 0) ? total_written : ret;
        }

        memcpy((char*)blk_ptr + block_offset, (char*)buf + total_written, chunk);

        cache_release_block(file_system_cache, blk_ptr, 1);

        total_written += chunk;

    }

    lock_release(&ktfs_master->ktfs_lock);

    return total_written;


}

long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len)
{


    struct ktfs_file * file = (void*)io - offsetof(struct ktfs_file, io);

    if (pos >= file->fsize)
        return 0;
    if (len < 0)
        return -EINVAL;

    if (pos + len > file->fsize) {
        len = file->fsize - pos;
    }

    struct ktfs_inode file_inode;

    lock_acquire(&ktfs_master->ktfs_lock);
    
    int ret = get_inode(file->ktfs_dir_entry.inode, &file_inode, 0);

    if(ret < 0){
        lock_release(&ktfs_master->ktfs_lock);
        return ret;
    }

    int block_index = 0;

    int block_offset = 0;

    long total_read = 0;

    while(total_read < len){

        unsigned long long curr_offset = pos + total_read;

        block_index = curr_offset / 512;

        block_offset = curr_offset % 512;

        long remaining = len - total_read;

        long chunk = 512 - block_offset;

        if(chunk > remaining){
            chunk = remaining;
        }

        uint32_t curr_block_num = ktfs_get_data_block(&file_inode, block_index);

        if(curr_block_num == -1) {
            // means no block allocated do a partial of zeroes ig
            memset((char*)buf + total_read, 0, chunk);
            total_read += chunk;
            continue;
        }

        void *blk_ptr;
        ret = cache_get_block(file_system_cache, (curr_block_num + ktfs_master->data_start_block) * KTFS_BLKSZ, (void**)&blk_ptr);
        if (ret < 0) {
            // partial read so far is total_read
            lock_release(&ktfs_master->ktfs_lock);
            return (total_read > 0) ? total_read : ret;
        }

        memcpy((char*)buf + total_read, (char*)blk_ptr + block_offset, chunk);

        cache_release_block(file_system_cache, blk_ptr, 0);

        total_read += chunk;

    }

    lock_release(&ktfs_master->ktfs_lock);

    return total_read;

}

int find_free_data_block(void)
{
    uint32_t bits_per_block = KTFS_BLKSZ * 8;

    for(uint32_t bmp_block = ktfs_master->bitmap_start_block; bmp_block < ktfs_master->data_start_block; ++bmp_block){
        uint8_t * block;
        int ret = cache_get_block(file_system_cache, bmp_block * KTFS_BLKSZ, (void**)&block);
        if(ret < 0) return ret;

        for(int i = 0; i < KTFS_BLKSZ; ++i){
            if(block[i] == 0xFF) continue;

            for(int j = 0; j < 8; ++j){
                if(!(block[i] & (1u << j))){
                    block[i] |= (1u << j);
                    int page_index = bmp_block - ktfs_master->bitmap_start_block;
                    int block_idx = page_index * bits_per_block + i*8 + j;
                    cache_release_block(file_system_cache, block, 1);
                    return block_idx;
                }
            }
        }
        cache_release_block(file_system_cache, block, 0);
    }

    return -1;
}

static int zero_block(uint32_t blk_no)
{
    void *p;
    int ret = cache_get_block(file_system_cache, (blk_no + ktfs_master->data_start_block) * KTFS_BLKSZ, (void**)&p);
    if (ret < 0) return ret;
    memset(p, 0, KTFS_BLKSZ);
    cache_release_block(file_system_cache, p, 1);
    return 0;
}



int ktfs_cntl(struct io *io, int cmd, void *arg)
{
    struct ktfs_file * file = (void*)io - offsetof(struct ktfs_file, io);
    switch (cmd) {
        case IOCTL_GETBLKSZ:
            return 1;
    
        case IOCTL_GETEND:
            // Return the file size via *(uint64_t*)arg
            if (!arg) {
                return -EINVAL;  // must provide a pointer
            }
            *(unsigned long long*)arg = file->fsize;
            return 0;
        
        case IOCTL_SETEND:
            unsigned long long new_size = *(unsigned long long *)arg;

            struct ktfs_inode inode;

            lock_acquire(&ktfs_master->ktfs_lock);

            int ret = get_inode(file->ktfs_dir_entry.inode, &inode, 0);

            if(ret < 0){
                lock_release(&ktfs_master->ktfs_lock);
                return ret;
            }


            if(new_size < inode.size || new_size > 16844288){
                lock_release(&ktfs_master->ktfs_lock);
                return -EINVAL;
            }


            unsigned old_blocks = (inode.size   + KTFS_BLKSZ-1) / KTFS_BLKSZ;
            unsigned new_blocks = (new_size     + KTFS_BLKSZ-1) / KTFS_BLKSZ;
            // unsigned blocks_needed = new_blocks - old_blocks;      //  can be zero 


            for(unsigned b = old_blocks; b < new_blocks; ++b){
                uint32_t data_blk = find_free_data_block() - ktfs_master->data_start_block; // find_free_data_block returns global block number not data relative
                if(data_blk == -1){
                    lock_release(&ktfs_master->ktfs_lock);
                    return -1;
                }

                void* ptr;
                ret = cache_get_block(file_system_cache, (data_blk + ktfs_master->data_start_block)*KTFS_BLKSZ, (void**)&ptr);
                if(ret < 0){
                    lock_release(&ktfs_master->ktfs_lock);
                    return ret;
                }
                memset(ptr, 0, KTFS_BLKSZ);
                cache_release_block(file_system_cache, ptr, 1);

                if(b < KTFS_NUM_DIRECT_DATA_BLOCKS){
                    inode.block[b] = data_blk;
                }
                else if(b < KTFS_NUM_DIRECT_DATA_BLOCKS + 128){
                    if(inode.indirect == 0){
                        inode.indirect = find_free_data_block() - ktfs_master->data_start_block;
                        if(inode.indirect == -1) return -1;
                        zero_block(inode.indirect);
                    }
                    uint32_t *indirect;
                    ret = cache_get_block(file_system_cache, (inode.indirect + ktfs_master->data_start_block)*KTFS_BLKSZ, (void**)&indirect);
                    if(ret < 0){
                        lock_release(&ktfs_master->ktfs_lock);
                        return ret;
                    }
                    indirect[b - KTFS_NUM_DIRECT_DATA_BLOCKS] = data_blk;

                    cache_release_block(file_system_cache, indirect, 1);
                }
                else{
                    unsigned rel = b - (KTFS_NUM_DIRECT_DATA_BLOCKS + 128);
                    unsigned dind_index = rel / (128*128); // either 0 or 1
                    unsigned inside_dind = rel % (128*128);
                    unsigned top_index = inside_dind / 128;
                    unsigned bottom_index = inside_dind % 128;

                    if(inode.dindirect[dind_index] == 0){
                        inode.dindirect[dind_index] = find_free_data_block() - ktfs_master->data_start_block;
                        if(inode.dindirect[dind_index] == -1){
                            lock_release(&ktfs_master->ktfs_lock);
                            return -1;
                        }

                        zero_block(inode.dindirect[dind_index]);
                    }

                    uint32_t * dind;
                    ret = cache_get_block(file_system_cache, (inode.dindirect[dind_index] + ktfs_master->data_start_block)*KTFS_BLKSZ, (void**)&dind);
                    if(ret < 0){
                        lock_release(&ktfs_master->ktfs_lock);
                        return ret;
                    }
                    if(dind[top_index] == 0){
                        dind[top_index] = find_free_data_block() - ktfs_master->data_start_block;
                        if(dind[top_index] == -1){
                            cache_release_block(file_system_cache, dind, 0);
                            lock_release(&ktfs_master->ktfs_lock);
                            return -1;
                        }
                        zero_block(dind[top_index]);
                    }

                    uint32_t *ind;

                    ret = cache_get_block(file_system_cache, (dind[top_index] + ktfs_master->data_start_block)*KTFS_BLKSZ, (void**)&ind);
                    if(ret < 0){
                        cache_release_block(file_system_cache, dind, 1);
                        lock_release(&ktfs_master->ktfs_lock);
                        return ret;
                    }

                    ind[bottom_index] = data_blk;

                    cache_release_block(file_system_cache, ind, 1);
                    cache_release_block(file_system_cache, dind, 1);
                }
            }

            inode.size = new_size;
            put_inode(file->ktfs_dir_entry.inode, &inode);

            file->fsize = new_size;
            
            lock_release(&ktfs_master->ktfs_lock);

            return 0;

        default:
            return -ENOTSUP;
        }
}


int ktfs_flush(void)
{
    int ret = cache_flush(file_system_cache);

    return ret;
}

int find_free_inode(void)
{
    int inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode);

    struct ktfs_inode zero_inode;
    memset(&zero_inode, 0, sizeof zero_inode);

    for (int blk = ktfs_master->inode_start_block;
         blk < ktfs_master->data_start_block;
         ++blk)
    {
        struct ktfs_inode *inode_block;
        int ret = cache_get_block(file_system_cache,
                    blk * KTFS_BLKSZ, (void**)&inode_block);
        if (ret < 0) return ret;

        for (int i = 0; i < inodes_per_block; ++i) {
            if (memcmp(&inode_block[i], &zero_inode, sizeof zero_inode) == 0) {
                int inode_num = (blk - ktfs_master->inode_start_block) *
                                inodes_per_block + i;
                cache_release_block(file_system_cache, inode_block, 0);
                return inode_num;
            }
        }
        cache_release_block(file_system_cache, inode_block, 0);
    }
    return -1;          /* or -1 if you prefer */
}



int ktfs_create	(const char * name){

    if(name == NULL){
        return -EINVAL;
    }

    uint16_t filler;

    lock_acquire(&ktfs_master->ktfs_lock);

    int ret = find_inode_by_name(name, &filler, 0);

    if(ret != -7){
        lock_release(&ktfs_master->ktfs_lock);
        return -EINVAL;
    }

    struct ktfs_inode root;

    ret = get_inode(ktfs_master->superblock.root_directory_inode, &root, 0);

    if(ret < 0){
        lock_release(&ktfs_master->ktfs_lock);
        return ret;
    }

    if(root.size >= 3 * KTFS_BLKSZ){ // root_directory file is alr full with 96 dentries
        lock_release(&ktfs_master->ktfs_lock);
        return -1;
    }

 
    int direct_block = root.size / KTFS_BLKSZ;

    if(direct_block > 0){
        if(root.block[direct_block] == 0){
            root.block[direct_block] = find_free_data_block();
            if(root.block[direct_block] == -1){
                lock_release(&ktfs_master->ktfs_lock);
                return -1;
            }
        }
    }

    int block_offset = root.size % KTFS_BLKSZ;

    int dentry_index = block_offset / sizeof(struct ktfs_dir_entry);

    struct ktfs_dir_entry * dir_block;

    ret = cache_get_block(file_system_cache, (root.block[direct_block] + ktfs_master->data_start_block)*KTFS_BLKSZ, (void**)&dir_block);

    if(ret < 0){
        lock_release(&ktfs_master->ktfs_lock);
        return ret;
    }



    int new_inode_num = find_free_inode();
    if (new_inode_num < 0) {
        // release the block we read, not dirty
        cache_release_block(file_system_cache, dir_block, 0);
        lock_release(&ktfs_master->ktfs_lock);
        return new_inode_num; // e.g. -ENOSPC if no free inode
    }


    struct ktfs_inode new_file_inode;
    memset(&new_file_inode, 0, sizeof(new_file_inode));
    new_file_inode.size = 0;


    ret = put_inode(new_inode_num, &new_file_inode);

    if (ret < 0) {
        // Optionally free the inode in your bitmap if needed
        cache_release_block(file_system_cache, dir_block, 0);
        lock_release(&ktfs_master->ktfs_lock);
        return ret;
    }

    struct ktfs_dir_entry addition;

    memset(&addition, 0, sizeof(addition));

    strncpy(addition.name, name, KTFS_MAX_FILENAME_LEN);

    addition.inode = (uint16_t)new_inode_num;

    dir_block[dentry_index] = addition;

    cache_release_block(file_system_cache, dir_block, 1);

    root.size += sizeof(struct ktfs_dir_entry);

    ret = put_inode(ktfs_master->superblock.root_directory_inode, &root);
    if (ret < 0) {
        lock_release(&ktfs_master->ktfs_lock);
        return ret; 
    }

    lock_release(&ktfs_master->ktfs_lock);

    return 0;

}


int ktfs_delete (const char * name)
{

    if(name == NULL){
        return -EINVAL;
    }

    for(int i = 0; i < MAX_OPEN_FILES; i++){
        if(open_files[i].in_use == 1){
            if(strcmp(open_files[i].ktfs_dir_entry.name, name) == 0){
                ktfs_close(&open_files[i].io);
            }
        }
    }

    uint16_t inode_number;

    lock_acquire(&ktfs_master->ktfs_lock);

    int ret = find_inode_by_name(name, &inode_number, 1);

    if(ret == -ENOENT){
        lock_release(&ktfs_master->ktfs_lock);
        return -1;
    }

    struct ktfs_inode inode;

    get_inode(inode_number, &inode, 1);

    int num_blocks = (inode.size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;

    int blocks_cleared = 0;

    int byte_index = 0;

    int bit_offset = 0;

    int block_number = 0;

    ret = 0;
    
    int bit_in_block = 0;

    //clear the direct data blocks

    for(int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS && blocks_cleared < num_blocks; i++){
        block_number = (inode.block[i] + ktfs_master->data_start_block) / (KTFS_BLKSZ * 8);

        bit_in_block = (inode.block[i] + ktfs_master->data_start_block) % (KTFS_BLKSZ * 8);

        byte_index = bit_in_block / 8;
        bit_offset = bit_in_block % 8;

        uint8_t * byte_ptr;

        ret = cache_get_block(file_system_cache, (block_number + ktfs_master->bitmap_start_block)*KTFS_BLKSZ, (void**)&byte_ptr);

        if(ret < 0){
            lock_release(&ktfs_master->ktfs_lock);
            return ret;
        }

        byte_ptr[byte_index] &= ~(1 << bit_offset); // little ender dragon (endian)

        cache_release_block(file_system_cache, byte_ptr, 1);
        
        blocks_cleared++;
    }



    // clear the indirect blocks type shi

    if (inode.indirect != 0) {
        uint32_t *ind_array;
        ret = cache_get_block(file_system_cache, (inode.indirect + ktfs_master->data_start_block) * KTFS_BLKSZ, (void **)&ind_array);

        if(ret < 0){
            lock_release(&ktfs_master->ktfs_lock);
            return ret;
        }

        for(int i = 0; i < 128 && blocks_cleared < num_blocks; i++){
            if(ind_array[i] == 0) continue;
            
            block_number = (ind_array[i] + ktfs_master->data_start_block) / (KTFS_BLKSZ * 8);
            
            bit_in_block = (ind_array[i] + ktfs_master->data_start_block) % (KTFS_BLKSZ *8);

            byte_index = bit_in_block / 8;
            bit_offset = bit_in_block % 8;

            uint8_t * byte_ptr;

            ret = cache_get_block(file_system_cache, (block_number + ktfs_master->bitmap_start_block)*KTFS_BLKSZ, (void**)&byte_ptr);

            if(ret < 0){
                cache_release_block(file_system_cache, ind_array, 0);
                lock_release(&ktfs_master->ktfs_lock);
                return ret;
            }

            byte_ptr[byte_index] &= ~(1 << bit_offset);

            cache_release_block(file_system_cache, byte_ptr, 1);

            blocks_cleared++;
        }

        block_number = (inode.indirect + ktfs_master->data_start_block) / (KTFS_BLKSZ * 8);
        bit_in_block = (inode.indirect + ktfs_master->data_start_block) % (KTFS_BLKSZ * 8);
        byte_index = bit_in_block / 8;
        bit_offset = bit_in_block % 8;
    
        uint8_t *byte_ptr;
        ret = cache_get_block(file_system_cache, (block_number + ktfs_master->bitmap_start_block) * KTFS_BLKSZ, (void**)&byte_ptr);
        if (ret < 0){
            lock_release(&ktfs_master->ktfs_lock);
            return ret;
        }
        byte_ptr[byte_index] &= ~(1 << bit_offset);

        cache_release_block(file_system_cache, byte_ptr, 1);

        cache_release_block(file_system_cache, ind_array, 0);
    }

    

    //clear dindirect blocks now frfr

    for(int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS && blocks_cleared < num_blocks; i++){
        if (inode.dindirect[i] == 0) continue; 

        uint32_t *dind_array;
        ret = cache_get_block(file_system_cache, (inode.dindirect[i] + ktfs_master->data_start_block) * KTFS_BLKSZ, (void **)&dind_array);

        if(ret < 0){
            lock_release(&ktfs_master->ktfs_lock);
            return ret;
        }

        for(int j = 0; j < 128 && blocks_cleared < num_blocks; j++){
            if (dind_array[j] == 0) continue;
            uint32_t * ind_array;

            ret = cache_get_block(file_system_cache, (dind_array[j] + ktfs_master->data_start_block) * KTFS_BLKSZ,(void **)&ind_array);
            if (ret < 0) {
                lock_release(&ktfs_master->ktfs_lock);
                cache_release_block(file_system_cache, dind_array, 0);
                return ret;
            }

            for (int k = 0; k < 128 && blocks_cleared < num_blocks; k++) {
                if (ind_array[k] == 0) continue;
    
                block_number = (ind_array[k] + ktfs_master->data_start_block) / (KTFS_BLKSZ * 8);
                bit_in_block = (ind_array[k] + ktfs_master->data_start_block) % (KTFS_BLKSZ * 8);
                byte_index = bit_in_block / 8;
                bit_offset = bit_in_block % 8;
    
                uint8_t *byte_ptr;
                ret = cache_get_block(file_system_cache,(block_number + ktfs_master->bitmap_start_block) * KTFS_BLKSZ, (void **)&byte_ptr);
                if (ret < 0) {
                    cache_release_block(file_system_cache, ind_array, 0);
                    cache_release_block(file_system_cache, dind_array, 0);
                    lock_release(&ktfs_master->ktfs_lock);
                    return ret;
                }
    
                byte_ptr[byte_index] &= ~(1 << bit_offset);
                cache_release_block(file_system_cache, byte_ptr, 1);
    
                blocks_cleared++;
            }
            //clear the intermediate indirect blocks
            block_number = (dind_array[j] + ktfs_master->data_start_block) / (KTFS_BLKSZ * 8);
            bit_in_block = (dind_array[j] + ktfs_master->data_start_block) % (KTFS_BLKSZ * 8);
            byte_index = bit_in_block / 8;
            bit_offset = bit_in_block % 8;
            uint8_t *byte_ptr;
            ret = cache_get_block(file_system_cache, (block_number + ktfs_master->bitmap_start_block) * KTFS_BLKSZ, (void**)&byte_ptr);
            
            if (ret < 0){
                lock_release(&ktfs_master->ktfs_lock);
                return ret;
            }
            byte_ptr[byte_index] &= ~(1 << bit_offset);

            cache_release_block(file_system_cache, byte_ptr, 1);
            cache_release_block(file_system_cache, ind_array, 0);
        }
        //clear the dindirect blocks themselves
        block_number = (inode.dindirect[i] + ktfs_master->data_start_block) / (KTFS_BLKSZ * 8);
        bit_in_block = (inode.dindirect[i] + ktfs_master->data_start_block) % (KTFS_BLKSZ * 8);
        byte_index = bit_in_block / 8;
        bit_offset = bit_in_block % 8;

        uint8_t *byte_ptr;
        ret = cache_get_block(file_system_cache, (block_number + ktfs_master->bitmap_start_block) * KTFS_BLKSZ, (void**)&byte_ptr);
        
        if (ret < 0){
            lock_release(&ktfs_master->ktfs_lock);
            return ret;
        }
        byte_ptr[byte_index] &= ~(1 << bit_offset);
        cache_release_block(file_system_cache, byte_ptr, 1);
        cache_release_block(file_system_cache, dind_array, 0);
    }
    lock_release(&ktfs_master->ktfs_lock);
    return 0;
}


    

