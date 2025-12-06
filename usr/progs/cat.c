#include "syscall.h"
#include "shell.h"
#include "string.h"

// we will wrap around with this, I hope AG blesses us
#define BUF_SIZE 1024

void print_file(int fd)
{
    char print[BUF_SIZE];
    int br, bw = 0;
    // while(bytes_read != wrap_around || bytes_read != 0) // keep going untill we filled up or we empty baaaka
    // lol baaaka this is a lot easier
    for (;;)
    {
        br = _read(fd, print, BUF_SIZE);
        if (br <= 0) return;

        while (br > 0)
        {
            bw = _write(STDOUT, print, br);
            if (bw < 0) return ;
            br -= bw;
        }
        
        // memset(print, 0, wrap_around);
    }

    // // keep going until we write everything
    // while(bytes_wrote < bytes_read)
    // {
    //     int bw = _write(STDOUT, print, bytes_read);
    //     if (bw < 0)
    //     {
    //         printf("bitch 3 \n");
    //         return;
    //     }
    //     bytes_wrote += bw;
    // }
}

void main(int argc, char *argv[]) {

    if (argc == 1) {
        print_file(STDIN);
        _exit();
    }

    // we dont print file name
    for (int i = 1; i < argc; i++)
    {
        int fd = _open(-1, argv[i]);
        if (fd < 0) { 
            // printf("bitch \r\n");
            continue;
        }
        print_file(fd);
        _close(fd);
    }
    _exit();
}