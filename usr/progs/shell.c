#include "syscall.h"
#include "string.h"
#include "shell.h"

#define BUFSIZE 1024
#define MAXARGS 8

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

int parse(char* buf, char** argv) {
	// FIXME
	// feel free to change this function however you see fit

	int argc = 0;
	char temp;
	char *head, *end;
	head = buf;

	for(;;) { // find each argv
		while(*head == ' ') head++;
		argv[argc++] = head;
		end = find_terminator(head);

		// inner loop handles all file redirection
		// it may be redirected multiple times
		for(;;) {
			temp = *end;
			*end = '\0';
			switch(temp) {
				case ' ':
					while(*(++head) == ' ') ;
					continue;

				case '\0':
					return argc;

				case FOUT:
					// FIXME
					continue;
					
				case FIN:
					// FIXME
					continue;

				case PIPE:
					// FIXME
					break;

				default:
					*head = temp;
					break;
			}
			break;
		}

	}
}

int main()
{
    char buf[BUFSIZE];
	int argc;
	char* argv[MAXARGS + 1]; 

  	_open(CONSOLEOUT, "dev/uart1");		// console device
	_close(STDIN);              		// close any existing stdin
	_uiodup(CONSOLEOUT, STDIN);      	// stdin from console
	_close(STDOUT);              		// close any existing stdout
	_uiodup(CONSOLEOUT, STDOUT);     	// stdout to console

	printf("Starting 391 Shell\n");

	for (;;)
	{
		printf("LUMON OS> ");
		getsn(buf, BUFSIZE - 1);

		if (0 == strcmp(buf, "exit"))
		_exit();

		// FIXME
		// Call your parse function and exec the user input
	}
}