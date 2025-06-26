// conf.h - Compile-time configuration constants
//
// Copyright (c) 2025 University of Illinois
// SPDX-License-identifier: NCSA
//

// QEMU-BASED CONSTANTS
//

#ifndef RAM_SIZE
#ifndef RAM_SIZE_MB
#define RAM_SIZE ((size_t)8*1024*1024)
#else
#define RAM_SIZE ((size_t)RAM_SIZE_MB*1024*1024)
#endif
#endif

#define RAM_START_PMA 0x80000000UL
#define RAM_START ((void*)RAM_START_PMA)
#define RAM_END_PMA (RAM_START_PMA+RAM_SIZE)
#define RAM_END (RAM_START+RAM_SIZE)

// MMIO addresses

#define UART0_MMIO_BASE 0x10000000UL // PMA
#define UART1_MMIO_BASE 0x10000100UL // PMA
#define UART_MMIO_BASE(i) \
    (UART0_MMIO_BASE + (i)*(UART1_MMIO_BASE-UART0_MMIO_BASE))
#define UART0_INTR_SRCNO 10

#define VIRTIO0_MMIO_BASE 0x10001000UL // PMA
#define VIRTIO1_MMIO_BASE 0x10002000UL // PMA
#define VIRTIO_MMIO_BASE(i) \
    (VIRTIO0_MMIO_BASE + (i)*(VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE))
#define VIRTIO0_INTR_SRCNO 1

#ifndef PLIC_MMIO_BASE
#define PLIC_MMIO_BASE 0x0C000000L
#endif

#define TIMER_FREQ 10000000UL // qemu/include/hw/intc/riscv_aclint.h

#define PLIC_SRC_CNT 96 // QEMU VIRT_IRQCHIP_NUM_SOURCES
#define PLIC_CTX_CNT 2

#define RTC_MMIO_BASE 0x00101000L

// KERNEL CONFIGURATION
//

#ifndef UMEM_START_VMA
#define UMEM_START_VMA 0x0C0000000UL
#endif

#ifndef UMEM_END_VMA
#define UMEM_END_VMA 0x100000000UL
#endif

#if UMEM_END_VMA <= UMEM_START_VMA
#error "UMEM_END_VMA <= UMEM_START_VMA"
#endif

#define UMEM_START ((void*)UMEM_START_VMA)
#define UMEM_END ((void*)UMEM_END_VMA)
#define UMEM_SIZE (UMEM_END - UMEM_START)

// Maximum number of devices

#ifndef NDEV
#define NDEV 16
#endif

// Number of external interrupt sources

#ifndef NIRQ
#define NIRQ PLIC_SRC_CNT
#endif

// Maximum number of threads

#ifndef NTHR
#define NTHR 32
#endif

// Maximum number of processes

#ifndef NPROC
#define NPROC 16
#endif

// Heap allocator alignment

#ifndef HEAP_ALIGN
#define HEAP_ALIGN 16
#endif

// Interrupt priorities

#define UART_INTR_PRIO 3
#define VIOBLK_INTR_PRIO 1
#define VIOCONS_INTR_PRIO 2
#define VIORNG_INTR_PRIO 1
#define VIOGPU_INTR_PRIO 2

// Maximum number of open io objects

#define PROCESS_IOMAX 16

// Capacity of block cache

#define CACHE_CAPACITY 64 // must be power of two

// KERNEL FEATURES
//

#if 1 // support for passing command-line arguments to exec'd process
#define WITH_ARGV
#endif
