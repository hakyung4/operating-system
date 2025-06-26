// #include <stddef.h>
// #include "syscall.h"

// void main(void) {
//     char * argv[] = { "trek_cp2", NULL };
//     int result;
//     int k = 0;

//     // Workaround for _wait(0) in main thread not returning -ECHILD because of
//     // kernel threads
//     if (_wait(_fork()) > 0)
//         return;
    
//     _iodup(0, 1); // output is nullio also
//     _fsopen(3, argv[0]);

//     // Attach shell to each uart except uart0
//     for (;;) {
//         result = _devopen(2, "uart", ++k);
//         if (result < 0)
//             break;
        
//         if (_fork() == 0)
//             _exec(3, 1, argv);
//         _close(2);
//     }

//     _close(3);

//     while (_wait(0) > 0)
//         continue;
// }
