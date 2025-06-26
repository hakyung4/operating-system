// #include "syscall.h" 

// void main(void) { 
//     int parent; 
//     int trekFd; 
    
// 	parent = _fork();
//     if (parent) {
//         trekFd = _fsopen(-1, "trek_cp2");
//         _devopen(2, "uart", 1); 
//         _exec(trekFd, 0, NULL); 
//     } else {
//         trekFd = _fsopen(-1, "fib");
//         _exec(trekFd, 0, NULL); 
//     }
// 	return;
// }
