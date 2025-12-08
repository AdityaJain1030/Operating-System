#include "syscall.h"
#include "string.h"

void main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (_fsdelete(argv[i]) < 0) {
            printf("failed to remove %s\n", argv[i]);
        }
    }
    _exit();
}