# Makefile
# 

PREFIX=riscv64-unknown-elf-
QEMU=qemu-system-riscv64

CC=$(PREFIX)gcc
AS=$(PREFIX)as
LD=$(PREFIX)ld
OBJCOPY=$(PREFIX)objcopy
OBJDUMP=$(PREFIX)objdump

OBJS = \
	assert.o \
	console.o \
	cache.o \
	thread.o \
	device.o \
	elf.o \
	error.o \
	excp.o \
	heap0.o \
	intr.o \
	io.o \
	plic.o \
	see.o \
	start.o \
	string.o \
	timer.o \
	trap.o \
	ktfs.o \
	thrasm.o \
	viorng.o \
	dev/virtio.o \
	dev/vioblk.o \
	rtc.o \
	uart.o \
	memory.o \
	process.o \
	syscall.o \
	
CFLAGS = -Wall -Werror=implicit-function-declaration
CFLAGS += -fno-omit-frame-pointer -ggdb3 -gdwarf-2
CFLAGS += -mcmodel=medany -fno-pie -no-pie -march=rv64imazicsr -mabi=lp64
CFLAGS += -fno-common -nostdlib -mno-relax -ffreestanding 
CFLAGS += -fno-asynchronous-unwind-tables -mno-riscv-attribute
CFLAGS += -I.

#CFLAGS += -DDEBUG -DTRACE # Everything!
#CFLAGS += -DMEMORY_DEBUG -DMEMORY_TRACE
#CFLAGS += -DHEAP_DEBUG -DHEAP_TRACE
#CFLAGS += -DEZFS_DEBUG -DEZFS_TRACE
#CFLAGS += -DLOCK_DEBUG -DLOCK_TRACE
#CFLAGS += -DMAIN_DEBUG -DMAIN_TRACE
#CFLAGS += -DTIMER_DEBUG -DTIMER_TRACE
#CFLAGS += -DCACHE_DEBUG -DCACHE_TRACE
#CFLAGS += -DKTFS_DEBUG -DKTFS_TRACE

ASFLAGS = -march=rv64imazicsr

LDFLAGS = -melf64lriscv

QEMUOPTS = -global virtio-mmio.force-legacy=false
QEMUOPTS += -machine virt -bios none -nographic

#viorng device
QEMUOPTS += -object rng-random,filename=/dev/urandom,id=rng0
QEMUOPTS += -device virtio-rng-device,rng=rng0

# vioblk device
QEMUOPTS += -drive file=ktfs.raw,id=blk0,if=none,format=raw,readonly=false
QEMUOPTS += -device virtio-blk-device,drive=blk0

# serial device
QEMUOPTS += -serial mon:stdio
QEMUOPTS += -serial pty
# QEMUOPTS += -serial pty

all: kernel.elf

kernel.elf: $(OBJS) main.o blob.o
	$(LD) $(LDFLAGS) -T kernel.ld -o $@ $^

run: kernel.elf
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $<

debug: kernel.elf
	$(QEMU) $(QEMUOPTS) -m 8M -kernel $< -S -s

	
BLOB_OBJCOPY_FLAGS = \
	--add-section .rodata.blob=blob.raw \
	--set-section-flags .rodata.blob=alloc,contents,load,readonly

blob.o:
	echo .end | $(AS) $(ASFLAGS) -o blob.o
	[ ! -f blob.raw ] || $(OBJCOPY) $(BLOB_OBJCOPY_FLAGS) $@

clean:
	rm -rf *.o dev/*.o test/*.o demo/*.o *.elf