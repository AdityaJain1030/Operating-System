#include "syscall.h"
#include "string.h"

void main(int argc, char *argv[]) {
    if (argc == 1) _exit();

    for (int i = 1; i < argc; i++) {
        if (_fscreate(argv[i]) < 0) {
            printf("failed to create write %s\n", argv[i]);
        }
    }

    _exit();
}