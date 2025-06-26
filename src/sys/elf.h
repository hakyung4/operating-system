// elf.c - ELF file loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _ELF_H_
#define _ELF_H_

#include "io.h"

extern int elf_load(struct io * elfio, void (**eptr)(void));

#endif // _ELF_H_
