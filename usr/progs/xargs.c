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
	char temp;
	char *head, *end;
	head = buf;

	for(;;) { // find each argv
		while(*head == ' ') head++;
		// support newlines
		//if (*head == '\0' || *head == '\n') break;

		if (argc >= *rem_args) break;
		argv[argc++] = head;
		end = find_terminator(head);
        //printf("head: %s\n",head);
        if (*end == '\0') break;
		*end = '\0';
		head = end+1;
		}

    // we dont mess with this
	argv[argc] = NULL;

    // for (int i = 0; i < argc; i++){
    //    printf("%s\n", argv[i]);
    // }
    *rem_args -= argc;
	return argc;
}


void main(int argc, char** argv) {
    char read[BUFSIZE + 1];
    int br = 0;
    
    char * xargv[MAXARGS + 1];

    int remaining_args = MAXARGS - (argc-1);


    //FIXME: early return for argc = 1 ??????

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
    // for (int i = 0; i < argc; i++){
    //     printf("argv[%d]", argv[i]);
    // }
    

    //stolen from shell
    int pid = _fork();
    if (pid < 0) {
        printf("ERROR: Failed to start process with code %d", pid);
        _exit();
    }
    

    // the lion dosent concern himself with setup
    if (pid != 0) {
        _wait(pid);
        _exit();
    }



    for (int i = 1; i < argc; i++){ //NOTE under this implementation argc will always be less than maxargs

        xargv[i-1] = argv[i];
        //printf("argv[%d]: %s\n", i-1, xargv[i-1]);
    }
    
	char name[100]; // same logic as when we do a kernel copy
	if (strchr(xargv[0], '/') == NULL)  // dosent contain a path proper, see docs for why we use this cond
		snprintf(name, 100, "c/%s", xargv[0]);
	else
		snprintf(name, 100, "%s", xargv[0]);
		
    //printf("name: %s\n", name);
	int rett = _open(-1, name);
    if (rett < 0)
    {
        printf("bad cmd file %s with error code %d \n", name, rett);
        _exit();
    }
    
    
    
    
    
    br = _read(STDIN, read, BUFSIZE);
    //printf("br: %d\n", br);
    if (br <= 0) _exit();
    //read[br] = '\0'; //ensure null termination
    
    //printf("argc: %d\n", argc);
    input_parse(&remaining_args, read, &xargv[argc-1]);

    
    int total_args = (MAXARGS) - remaining_args;
    //printf("total_args: %d\n", total_args);

    // for (int i = 0; i < total_args; i++){
    //     printf("xargv[%d]: %p\n", i, &xargv[i]);
    // }

    
    int ret = _exec(rett, total_args, xargv);

    printf("exec not working ...ret: %d\n", ret);
    
}
