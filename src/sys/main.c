// #include "conf.h"
// #include "console.h"
// #include "elf.h"
// #include "assert.h"
// #include "thread.h"
// #include "process.h"
// #include "memory.h"
// #include "fs.h"
// #include "io.h"
// #include "device.h"
// #include "rtc.h"
// #include "uart.h"
// #include "intr.h"
// #include "dev/virtio.h"
// #include "heap.h"
// #include "string.h"

// #define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
// extern char _kimg_end[]; 

// #define INIT_NAME "trek_wrapper"
// #define NUM_UARTS 3


// void main(void) {
//     struct io *blkio;
//     int result;
//     int i;

    
//     console_init();
//     devmgr_init();
//     intrmgr_init();
//     thrmgr_init();
//     memory_init();
//     procmgr_init();
//     start_interrupter();



    
    
//     for (i = 0; i < NUM_UARTS; i++) // change for number of UARTs
//         uart_attach((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO+i);
        
//     rtc_attach((void*)RTC_MMIO_BASE);

//     for (i = 0; i < 8; i++) {
//         virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
//     }
//     enable_interrupts();

//     result = open_device("vioblk", 0, &blkio);
//     if (result < 0) {
//         kprintf("Error: %d\n", result);
//         panic("Failed to open vioblk\n");
//     }

//     result = fsmount(blkio);
//     if (result < 0) {
//         kprintf("Error: %d\n", result);
//         panic("Failed to mount filesystem\n");
//     }


//     // insert testcase below
//     // This test case will run fib and trek simultaneously. 
//     struct io *trekFibio;
//     result = fsopen("trek_wrapper", &trekFibio);
//     if (result < 0) {
//         kprintf(INIT_NAME ": %s; Unable to open\n");
//         panic("Failed to open trek_wrapper\n");
//     }
//     kprintf("asdf");
//     result = process_exec(trekFibio, 0, NULL);
//     kprintf("this should not be printed\n");
    
// }



#include "conf.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "process.h"
#include "memory.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "rtc.h"
#include "uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "heap.h"
#include "string.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 

void main(void) {
    struct io *blkio, *shellio;
    int result;
    int i;

    
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();
    start_interrupter();



    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);
    
    for (i = 0; i < 8; i++) {
        virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }

    result = open_device("vioblk", 0, &blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open vioblk\n");
    }

    result = fsmount(blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to mount filesystem\n");
    }

    result = open_device("uart", 1, &current_process()->iotab[2]);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open UART\n");
    }

    result = fsopen("shell.elf", &shellio);
    if (result < 0) panic("Failed to open shell.elf");

    kprintf("GOT HERE TYPE ISH");

    result = process_exec(shellio, 0, NULL);

    kprintf("GOT HERE TYPE SHIII");
    
    panic("Should not return from shell");
}
