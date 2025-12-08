#include "syscall.h"
#include "string.h"
#include "shell.h"

#define BUFSIZE 1024
#define MAXARGS 50

// helper function for parser
char* find_terminator(char* buf) {
    char* p = buf;
    while(*p) {
        switch(*p) {
            case ' ':
            case '\0': 
            case '\r': // fuck my stupid chungus life
            case '\t':
            case '\n':
                return p;
            default:
                p++;
                break;
        }
    }
    return p;
}

// add redirect in and redirect out files
int input_parse(int *rem_args, char* buf, char** argv) {
    // FIXME
    // feel free to change this function however you see fit

    int argc = 0;
    char *head, *end;
    head = buf;

    for(;;) { // find each argv
        while(*head == ' ' || *head == '\n' || *head =='\r' || *head =='\t') head++; // \r \r go away com back anothr day
        
        if (*head == '\0') break;

        if (argc >= *rem_args) break;
        
        argv[argc++] = head;
        end = find_terminator(head);
        
        if (*end == '\0') break;
        
        *end = '\0';
        head = end+1;
    }

    // we dont mess with this
    argv[argc] = NULL;
    
    *rem_args -= argc;
    return argc;
}


void main(int argc, char** argv) {
    char read[BUFSIZE + 1];
    int br = 0;
    char *xargv[MAXARGS + 1];
    int remaining_args = MAXARGS;

    // looping variation
    // if (argc == 1){ //case for only args coming from input
    //     for (;;){
    //         memset(read, 0, BUFSIZE);
    //         br = _read(STDIN, read, BUFSIZE);
    //         if (br <= 0) return;
    //
    //         input_parse(&remaining_args, read, xargv);
    //
    //         if ()
    //
    //         _exec();
    //
    //
    //     }
    // }
    
    //stolen from shell
    int pid = _fork();
    if (pid < 0) {
        printf("ERROR: Failed to start process with code %d\n", pid);
        _exit();
    }
    
    // the lion dosent concern himself with setup
    if (pid != 0) {
        _wait(pid);
        _exit();
    }

    // Child Logic
    
    int xargc = 0;
    
    // Copy arguments provided to xargs (skipping xargs itself)
    for (int i = 1; i < argc; i++) { // NOTE under this implementation argc will always be less than maxargs
        if (remaining_args > 0) {
            xargv[xargc++] = argv[i];
            remaining_args--;
        }
    }
    
    // Read from STDIN
    memset(read, 0, BUFSIZE + 1);
    br = _read(STDIN, read, BUFSIZE);
    
    if (br > 0) {
        read[br] = '\0'; // ensure null termination
        
        // Parse input and append to xargv
        int br = input_parse(&remaining_args, read, &xargv[xargc]);
        xargc += br;
    }
    
    xargv[xargc] = NULL; // Ensure final null termination
    
    if (xargc == 0) _exit(); // Nothing to execute

    char name[100]; // same logic as when we do a kernel copy
    if (strchr(xargv[0], '/') == NULL)  // dosent contain a path proper
        snprintf(name, 100, "c/%s", xargv[0]);
    else
        snprintf(name, 100, "%s", xargv[0]);
        
    int rett = _open(-1, name);
    if (rett < 0)
    {
        printf("bad cmd file %s with error code %d \n", name, rett);
        _exit();
    }
    
    int ret = _exec(rett, xargc, xargv);

    printf("exec not working cro: %d\n", ret);
    _exit();
}