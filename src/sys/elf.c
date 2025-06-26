// elf.c - ELF file loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "elf.h"
#include "conf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "assert.h"
#include "ktfs.h"
#include "error.h"

#include <stdint.h>

// Offsets into e_ident

#define EI_CLASS        4   
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8   
#define EI_PAD          9  


// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE     0
#define EV_CURRENT  1

// ELF header e_type values

enum elf_et {
    ET_NONE = 0,
    ET_REL,
    ET_EXEC,
    ET_DYN,
    ET_CORE
};

struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff; 
    uint32_t e_flags; 
    uint16_t e_ehsize; 
    uint16_t e_phentsize; 
    uint16_t e_phnum; 
    uint16_t e_shentsize; 
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

enum elf_pt {
	PT_NULL = 0, 
	PT_LOAD,
	PT_DYNAMIC,
	PT_INTERP,
	PT_NOTE,
	PT_SHLIB,
	PT_PHDR,
	PT_TLS
};

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// define lower and upper bound for checking the address of sections
#define LOWER_BOUND 0x0C0000000UL
#define UPPER_BOUND 0x100000000UL

// ELF header e_machine values (short list)
#define  EM_RISCV   243

int elf_load(struct io * elfio, void (**eptr)(void)) {
    struct elf64_ehdr ehdr;

    long r = ioreadat(elfio, 0, &ehdr, sizeof(ehdr));
    if (r < 0) return r;
    if (r != sizeof(ehdr)) {
        return -EIO;
    }

    if (memcmp(ehdr.e_ident, "\x7F""ELF", 4) != 0) {
        return -EINVAL;
    }

    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        return -EBADFMT;
    }

    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        return -EBADFMT;
    }

    if (ehdr.e_type != ET_EXEC) {
        return -EBADFMT;
    }

    if (ehdr.e_machine != EM_RISCV) {
        return -EBADFMT;
    }

    if (ehdr.e_ident[EI_VERSION] != EV_CURRENT || ehdr.e_version != EV_CURRENT) {
        return -EBADFMT;
    }

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        struct elf64_phdr phdr;
        r = ioreadat(elfio, ehdr.e_phoff + i * ehdr.e_phentsize, &phdr, sizeof(phdr));
        if (r < 0) {
            return r;
        }
        if (r != sizeof(phdr)) {
            return -EIO;
        }

        if (phdr.p_type != PT_LOAD) {
            continue;
        }

        if (phdr.p_vaddr < LOWER_BOUND || (phdr.p_vaddr + phdr.p_memsz) > UPPER_BOUND) {
            return -EBADFMT;
        }

        int flags = PTE_R | PTE_W | PTE_U;

        if (!alloc_and_map_range((uintptr_t)phdr.p_vaddr, phdr.p_memsz, flags)) {
            return -ENOMEM;
        }

        r = ioreadat(elfio, phdr.p_offset, (void *)phdr.p_vaddr, phdr.p_filesz);
        if (r < 0) {
            return r;
        }
        if (r != phdr.p_filesz) {
            return -EIO;
        }

        if (phdr.p_flags & PF_X) {
            flags |= PTE_X;
        }
        if (phdr.p_flags & PF_W) {
            flags |= PTE_W;
        }
        if (phdr.p_flags & PF_R) {
            flags |= PTE_R;
        }

        set_range_flags((const void*)phdr.p_vaddr, phdr.p_memsz, flags);

        if (phdr.p_memsz > phdr.p_filesz) {
            memset((void *)(phdr.p_vaddr + phdr.p_filesz), 0, phdr.p_memsz - phdr.p_filesz);
        }
    }
    *eptr = (void (*)(void))ehdr.e_entry;
    return 0;
}