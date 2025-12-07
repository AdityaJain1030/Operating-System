#include "syscall.h"
#include "string.h"
#include "shell.h"

// most of wc is stolen from cat
#define BUF_SIZE 1024

// we abstract away this because im not sure
// what counts as a space
int space(char c) {
    return (c == ' ' || c == '\n');
}

int newline(char c)
{
    return (c == '\n');
}

void count(int fd, char* name)
{
    int words, lines, bytes = 0;
    char print[BUF_SIZE];
    int br = 0;

    for (;;) {
        br = _read(fd, print, BUF_SIZE);
        if (br < 0) return;
        if (br == 0) break;

        for (int i = 0; i < br; i++) {
            char c = print[i];
            
            bytes++;
            if (newline(c)) lines++;
            if (space(c)) words++;
        }
    }
    printf("%d\t%d\t%d\t%s\r", lines, words, bytes, name);
}

void main(int argc, char *argv[]) {
    // we dont print file name
    for (int i = 1; i < argc; i++)
    {
        int fd = _open(-1, argv[i]);
        if (fd < 0) { 
            continue;
        }
        count(fd, argv[i]);
        _close(fd);
        // if (i != argc) printf("\n");
    } 
    _exit();
}