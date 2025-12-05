#include "syscall.h"
#include "string.h"
#include <sys/syslimits.h>
#include "shell.h"

void print_parsed_command(char* buf, char** argv, char* readinf, char* readoutf, int argc) {
    printf("Parsed Command:\n");
    printf("  buf: '%s'\n", buf);
    printf(" argc: %d \n", argc);
    printf("  argv:\n");
    for (int i = 0; i < argc + 1; i++) {
        printf("    argv[%d]: '%s'\n", i, argv[i]);
    }
    if (readinf != NULL) {
        printf("  Input Redirection: '%s'\n", readinf);
    } else {
        printf("  Input Redirection: None\n");
    }
    if (readoutf != NULL) {
        printf("  Output Redirection: '%s'\n", readoutf);
    } else {
        printf("  Output Redirection: None\n");
    }
}