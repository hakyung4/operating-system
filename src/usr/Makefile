# Based on xv6-riscv Makefile; see LICENSE.

TOOLPREFIX=riscv64-unknown-elf-
QEMU=qemu-system-riscv64

CC=$(TOOLPREFIX)gcc
AS=$(TOOLPREFIX)as
LD=$(TOOLPREFIX)ld
OBJCOPY=$(TOOLPREFIX)objcopy
OBJDUMP=$(TOOLPREFIX)objdump

UMODE := 1

ULIB_OBJS = \
	io.o \
	string.o \
	syscall.o \
	heap.o

ULIB_LD = no_umode.ld

ifeq ($(UMODE), 1)
	ULIB_OBJS += start.o
	ULIB_LD = umode.ld
endif

ALL_TARGETS = \
	hello sysArg_test trek_wrapper

CFLAGS = -Wall -fno-omit-frame-pointer -ggdb3 -gdwarf-2
CFLAGS += -mcmodel=medany -fno-pie -no-pie -march=rv64g -mabi=lp64d
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -fno-asynchronous-unwind-tables
CFLAGS += -I.

ifeq ($(UMODE), 1)
	CFLAGS += -DUMODE
endif

all: $(ALL_TARGETS)

hello: $(ULIB_OBJS) hello.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

sysArg_test: $(ULIB_OBJS) sysArg_test.o | bin
	$(LD) -T $(ULIB_LD) -o bin/$@ $^

sysArg_test.o: sysArg_test.c
	$(CC) $(CFLAGS) -c -o $@ $<

trek_wrapper: $(ULIB_OBJS) trek_wrapper.o | bin
	$(LD) -T umode.ld -o bin/$@ $^

trek_wrapper.o: trek_wrapper.c
	$(CC) $(CFLAGS) -c -o $@ $<

bin: 
	mkdir $@

clean:
	rm -rf *.o *.elf *.asm
