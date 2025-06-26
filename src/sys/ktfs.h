#include <stdint.h>
#include "ioimpl.h"

#define KTFS_BLKSZ              512
#define KTFS_INOSZ              32
#define KTFS_DENSZ              16
#define KTFS_MAX_FILENAME_LEN        KTFS_DENSZ - sizeof(uint16_t) - sizeof(uint8_t)
#define KTFS_NUM_DIRECT_DATA_BLOCKS  3
#define KTFS_NUM_INDIRECT_BLOCKS     1
#define KTFS_NUM_DINDIRECT_BLOCKS    2

#define KTFS_FILE_IN_USE (1 << 0)
#define KTFS_FILE_FREE (0 << 0)

/*
Overall filesystem image layout

+------------------+
|   Superblock     |
+------------------+
|     Padding      |
+------------------+
|   Bitmap Blk 0   |
+------------------+
|   Bitmap Blk 1   |
+------------------+
|      ...         |
+------------------+
|   Inode Blk 0    |
+------------------+
|   Inode Blk 1    |
+------------------+
|      ...         |
+------------------+
|    Data Blk 0    |
+------------------+
|    Data Blk 1    |
+------------------+
|      ...         |
+------------------+

Imagining this layout as a struct, it would look like this:

struct filesystem {
    struct ktfs_superblock superblock;
    uint8_t padding[BLOCK_SIZE - sizeof(ktfs_superblock)];
    struct ktfs_bitmap bitmaps[];
    struct ktfs_inode inodes[];
    struct ktfs_data_block data_blocks[];
};

NOTE: The ((packed)) attribute is used to ensure that the struct is packed and 
there is no padding between the members or struct alignment requirements.
*/

// Superblock
struct ktfs_superblock {
    uint32_t block_count;
    uint32_t bitmap_block_count;
    uint32_t inode_block_count;
    uint16_t root_directory_inode;
} __attribute__((packed));

// Inode with indirect and doubly-indirect blocks
struct ktfs_inode {
    uint32_t size;                                  // Size in bytes
    uint32_t flags;                                 // File type, etc. (unused in MP3)
    uint32_t block[KTFS_NUM_DIRECT_DATA_BLOCKS];    // Direct block indices
    uint32_t indirect;                              // Indirect block index
    uint32_t dindirect[KTFS_NUM_DINDIRECT_BLOCKS];  // Doubly-indirect block indices
} __attribute__((packed));

// Directory entry
struct ktfs_dir_entry {
    uint16_t inode;                                         // Inode number
    char     name[KTFS_MAX_FILENAME_LEN+sizeof(uint8_t)];   // File name (plus null terminator)
} __attribute__((packed));

// Bitmap block
struct ktfs_bitmap {
    uint8_t bytes[KTFS_BLKSZ];
} __attribute__((packed));


struct ktfs_data_block {
    uint8_t data[KTFS_BLKSZ];
}__attribute__((packed));
