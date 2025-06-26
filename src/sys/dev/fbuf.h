// fbuf.h - ioctls for a frame buffer device (e.g. viogpu)
// 
// Copyright (c) 2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#define IOCTL_GETFBUF       100 // arg is void **
#define IOCTL_MAPFBUF       101 // arg is void **
#define IOCTL_GETFBCONF     102 // arg is struct fbuf_conf *
#define IOCTL_SETFBCONF     103 // arg is const struc fbuf_conf *

// Pixel types

#define FBUF_B8G8R8X8       2 // VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM

struct fbuf_conf {
    unsigned short width;
    unsigned short height;
    unsigned short pxtype; // e.g. FBUF_B8G8R8X8
};