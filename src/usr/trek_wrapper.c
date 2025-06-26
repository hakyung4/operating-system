#include "syscall.h" 

void main(void) { 
    int parent; 
    int trekFd; 
    trekFd = _fsopen(-1, "trek_cp2");
	parent = _fork();
    if (parent) {
        _devopen(2, "uart", 1); 
        _exec(trekFd, 0, NULL); 
    } else {
        _devopen(2, "uart", 2); 
        _exec(trekFd, 0, NULL); 
    }
	return;
}
