#include "syscall.h"
#include "string.h"
#include "shell.h"

#define BUFSIZE 1024
#define MAXARGS 100


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
int parse(char* buf, char** argv, char **readinf, char** readoutf) {
	// FIXME
	// feel free to change this function however you see fit

	int argc = 0;
	char temp;
	char *head, *end;
	head = buf;

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
					head++; // we didnt do this yet
					break;

				default:
					// *head = temp;
					break;
			}
			break;
		}

	}
    // we dont mess with this
	// argv[argc] = NULL;
	return argc;
}


void main(int argc, char** argv) {
    // implement this soon
}