#include "syscall.h"
#include "string.h"
#include <sys/syslimits.h>
#include "shell.h"

#define BUFSIZE 1024
#define MAXARGS 8

// #include "shell_utils.c"

// helper function for parser
char* find_terminator(char* buf) {
	char* p = buf;
	while(*p) {
		switch(*p) {
			case ' ':
			case '\0':
			case FIN:
			case FOUT:
			case PIPE:
				return p;
			default:
				p++;
				break;
		}
	}
	return p;
}

// add redirect in and redirect out files
int parse(char* buf, char** argv, char **readinf, char** readoutf, char** cont) {
	// FIXME
	// feel free to change this function however you see fit

	int argc = 0;
	char temp;
	char *head, *end;
	head = buf;

	// ok so to avoid upheaving a lot of parse infra we can cheat the system to handle pipes as indepdendent commands
	// we then just run parse individually on each command
	// cont will be the starting point of the next command (NULL if there is none)
	*cont = NULL;

	for(;;) { // find each argv
		while(*head == ' ') head++;
		// support newlines
		if (*head == '\0' || *head == '\n') break;

		if (argc >= MAXARGS) break;
		argv[argc++] = head;
		end = find_terminator(head);
		temp = *end;
		*end = '\0';
		head = end+1;
		// inner loop handles all file redirection
		// it may be redirected multiple times
		for(;;) {
			switch(temp) {
				case ' ':
					while (*head == ' ') head++;
					temp = *head;
					// while(*(head) == ' ') head++;
					// end = head;
					// temp = *end;
					continue;

				case '\0':
					argv[argc] = NULL;
					return argc;

				case FOUT:
					// FIXME
					// head++;
					while(*head != '\0')
					{
						if (*head != ' ') break;
						head += 1;
					}
					if (*head == '\0') break;
					end = find_terminator(head);
					*readoutf = head;
					temp = *end;
					*end = '\0';
					head = end + 1;

					// hopefully this shld work
					// head = next + 1; // we go to the next arg
					// if (*next == '\0') head -= 1; // if we are at end we overshot
					// *next = '\0'; // we do this so then the file uio reader
					// // dosent trip ... this was a bitch to debug
					continue;
					
				case FIN:
					// head++;
					while(*head != '\0')
					{
						if (*head != ' ') break;
						head += 1;
					}
					if (*head == '\0') break;
					end = find_terminator(head);
					*readinf = head;
					temp = *end;
					*end = '\0';
					head = end + 1;

					// head = next + 1; // we go to the next arg
					// if (*next == '\0') head -= 1; // if we are at end we overshot
					// *next = '\0'; // we do this so then the file uio reader
					// // dosent trip ... this was a bitch to debug
					continue;

				case PIPE:
					// FIXME
					// head++;
					if (*head == PIPE) head++;
					*cont = head;
					argv[argc] = NULL;
					return argc;

				default:
					// *head = temp;
					break;
			}
			break;
		}

	}
	argv[argc] = NULL;
	return argc;
}

void main(void)
{
    char buf[BUFSIZE];
	int argc;
	char* argv[MAXARGS + 1];
	char* readinf;
	char* readoutf;

	_open(CONSOLEOUT, "dev/uart1");		// console device
	_close(STDIN);              		// close any existing stdin
	_uiodup(CONSOLEOUT, STDIN);      	// stdin from console
	_close(STDOUT);              		// close any existing stdout
	_uiodup(CONSOLEOUT, STDOUT);     	// stdout to console

	printf("Starting 391 Shell\n");

	for (;;)
	{
		memset(buf, 0, BUFSIZE);
		printf("LUMON OS> ");
		getsn(buf, BUFSIZE - 1);

		if (0 == strcmp(buf, "exit")) _exit();
	
		char* cont = buf;
		int children[20];
		int childcount = 0;
		
		int pipe_in = -1;

		while (cont != NULL)
		{	
			readinf = NULL;
			readoutf = NULL; // reset every command 
			argc = parse(cont, argv, &readinf,  &readoutf, &cont);
			
			if (argc == 0) break;
			if (argc > ARG_MAX) break;

			// FIXME
			// Call your parse function and exec the user input
			// printf("%d", argc);
			
			// print out all the buf, argv, readinf, and readoutf in console
			// print_parsed_command(buf, argv, readinf, readoutf, argc);		
			// continue;
			

			// fork
			// WE FORK FIRST RIGHT AWAY... THIS IS ONE OF THE REASONS WE 
			// WANT CHILD TO RUN FIRST SO THAT WE CAN SETUP CHILD IN SHELL
			int pfd[2] = {-1, -1};
            if (cont != NULL) {
				int err = _pipe(&pfd[0], &pfd[1]);
				if (err < 0)
				{
					printf("failed to make pipe\n");
					break;
				}
            }

			int pid = _fork();
			if (pid < 0) {
				printf("ERROR: Failed to start process with code %d", pid);
				if (cont != NULL)
				{
					_close(pfd[0]);
					_close(pfd[1]);
				}
				break;
			}
			// what the child runs
			if (pid == 0)
			{
				// open files
				// for exec file prepend c if it is not alr there
				char name[100]; // same logic as when we do a kernel copy
				if (strchr(argv[0], '/') == NULL)  // dosent contain a path proper, see docs for why we use this cond
					snprintf(name, 100, "c/%s", argv[0]);
				else
					snprintf(name, 100, "%s", argv[0]);
				
				int rett = _open(-1, name);
				if (rett < 0)
				{
					printf("bad cmd file %s with error code %d \n", name, rett);
					_exit();
				}

				if (readinf != NULL)
				{
					_close(STDIN);
					int ret = _open(STDIN, readinf);
					if (ret < 0) {
						printf("bad input file %s with error code %d \n", readinf, ret);
						_exit();
					}
				}
				
				if (readoutf != NULL)
				{
					_close(STDOUT);
					// we can just delete and recreate the readoutf each time
					// instead of doing the bs below
					_fsdelete(readoutf);
					int ret = _fscreate(readoutf);
					if (ret < 0)
					{
						printf("bad output file create %s with error code %d \n", readoutf, ret);
						_exit();
					}
					ret = _open(STDOUT, readoutf);
					if (ret < 0)
					{
						printf("bad output file open %s with error code %d \n", readoutf, ret);
						_exit();
					}
				}
				// new, we have to change our output to the pipe out
				if (cont != NULL)
				{
					// bruh moment
					_close(pfd[1]);
					int err = _close(STDOUT);
					// printf("%d", err);
					err = _uiodup(pfd[0], STDOUT);
					// printf("%d\n", err);
					// im not fixing ts race contidion
					_usleep(10000);
					// for (int i = err; i < 100000000; i++)
					// {
					// 	err += 1;
					// }
					_close(pfd[0]);
				}

				// if pipe is in we change to this
				if (pipe_in != -1)
				{
					_close(STDIN);
					_uiodup(pipe_in, STDIN);
					_close(pipe_in);
				}

				_exec(rett, argc, argv);
				printf("Exec not working cro");
				_exit();

			}

			// we are in parent land now
			children[childcount] = pid;
			childcount += 1;

			if (pipe_in != -1) _close(pipe_in);
			pipe_in = pfd[1];
			_close(pfd[0]);
			// _close(pfd[1]);
		}
		_close(pipe_in);
		for (int i = 0; i < childcount; i ++)
		{
			_wait(0);
		}
	}
}