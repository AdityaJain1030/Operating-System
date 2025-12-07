#include "syscall.h"
#include "string.h"
#include "shell.h"

#define LS_BUFSZ 1024

void main (int argc, char *argv[]){

    char buf[LS_BUFSZ+1]; 
    if (argc == 1)
    {
        int fd = _open(-1, "");
        if (fd < 0)
        {
            printf("ls cannot access root\r");
            _exit();
        }
        while (1) {
            int br = _read(fd, buf, LS_BUFSZ);
            if (br <= 0) break;
            buf[br] = '\0'; // null terminate end of string
            printf("%s\n", buf);
        }
        _close(fd);
        _exit();
    }

    for (int i = 1; i < argc; i++){
        int fd = _open(-1, argv[i]);
        if (fd < 0) continue;

        int is_ktfs = 0;
        if (strcmp(argv[i], "c") == 0 || strcmp(argv[i], "c/")==0) is_ktfs = 1;

        while (1){
            int br = _read(fd, buf, LS_BUFSZ);
            if (br <= 0) break;

            if (!is_ktfs) buf[br] = '\0'; // null terminate end of string
            else buf[br-1] = '\0';
            printf("%s\n", buf);

            // we have hella newlines idt we need this
            // if (is_ktfs) dputs(STDOUT, "\r\n"); //because the other listings don't have it built in like ktfs
        }
        _close(fd);
    }
    // we have hella newlines idt we need this
    // dputs(STDOUT, "\n");
}
    

    // for (int i = 1; i < argc; i++){
    //     int fd = _open(-1, argv[i]);
    //     if (fd < 0) continue;
    //
    //
    //     if (strcmp(argv[i], "c") == 0) {
    //         _read(fd, buf, LS_BUFSZ);
    //         dprintf(STDOUT, "%s", buf);
    //
    //         dprintf(STDOUT, "\n");//idk I can't debug this
    //     }
    //     else {  //case for / and dev
    //
    //         while (1){
    //             int br = _read(fd, buf, LS_BUFSZ);
    //             if (bw =< 0) break;
    //
    //             dprintf(STDOUT, "%s\n", buf);//ok the idea is to print everything on a new line
    //                                                  //maybe the way I'm doing it rn is wrong though.
    //                                                  //u can tune it however u want
    //
    //             //note: if the previous impl requires more logic for the return
    //             //on the last print, just use dputs
    //         }
    //
    //
    //     }
    //
    //     dprintf(STDOUT,"\r"); 
//     }

    

// }
