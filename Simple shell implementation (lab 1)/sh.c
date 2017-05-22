#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "list.h"

#define PERM		(0644)		/* default permission rw-r--r-- */
#define MAXBUF		(512)		/* max length of input line. */
#define MAX_ARG		(100)		/* max number of cmd line arguments. */
#define MAX_PATH_LENGTH (100)	/* max path length */

typedef enum { 
	AMPERSAND, 			/* & */
	NEWLINE,			/* end of line reached. */
	NORMAL,				/* file name or command option. */
	INPUT,				/* input redirection (< file) */
	OUTPUT,				/* output redirection (> file) */
	PIPE,				/* | for instance: ls *.c | wc -l */
	SEMICOLON			/* ; */
} token_type_t;

static char*	progname;		/* name of this shell program. */
static char	input_buf[MAXBUF];	/* input is placed here. */
static char	token_buf[2 * MAXBUF];	/* tokens are placed here. */
static char*	input_char;		/* next character to check. */
static char*	token;			/* a token such as /bin/ls */

static list_t*	path_dir_list;		/* list of directories in PATH. */
static int	input_fd;		/* for i/o redirection or pipe. */
static int	output_fd;		/* for i/o redirection or pipe */

/* fetch_line: read one line from user and put it in input_buf. */
int fetch_line(char* prompt)
{
	int	c;
	int	count;

	input_char = input_buf;
	token = token_buf;

	printf("%s", prompt);
	fflush(stdout);

	count = 0;

	for (;;) {

		c = getchar();

		if (c == EOF)
			return EOF;

		if (count < MAXBUF)
			input_buf[count++] = c;

		if (c == '\n' && count < MAXBUF) {
			input_buf[count] = 0;
			return count;
		}

		if (c == '\n') {
			printf("too long input line\n");
			return fetch_line(prompt);
		}

	}
}

/* end_of_token: true if character c is not part of previous token. */
static bool end_of_token(char c)
{
	switch (c) {
	case 0:
	case ' ':
	case '\t':
	case '\n':
	case ';':
	case '|':
	case '&':
	case '<':
	case '>':
		return true;

	default:
		return false;
	}
}

/* gettoken: read one token and let *outptr point to it. */
int gettoken(char** outptr)
{
	token_type_t	type;

	*outptr = token;

	while (*input_char == ' '|| *input_char == '\t')
		input_char++;

	*token++ = *input_char;

	switch (*input_char++) {
	case '\n':
		type = NEWLINE;
		break;

	case '<':
		type = INPUT;
		break;
	
	case '>':
		type = OUTPUT;
		break;
	
	case '&':
		type = AMPERSAND;
		break;
	
	case '|':
		type = PIPE; 
		break;
	
	default:
		type = NORMAL;

		while (!end_of_token(*input_char))
			*token++ = *input_char++;
	}

	*token++ = 0; /* null-terminate the string. */
	
	return type;
}

/* error: print error message using formatting string similar to printf. */
void error(char *fmt, ...)
{
	va_list		ap;

	fprintf(stderr, "%s: error: ", progname);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	/* print system error code if errno is not zero. */
	if (errno != 0) {
		fprintf(stderr, ": ");
		perror(0);
	} else
		fputc('\n', stderr);

}

/* run_program: fork and exec a program. */
void run_program(char** argv, int argc, bool foreground, bool doing_pipe)
{
	/* you need to fork, search for the command in argv[0],
         * setup stdin and stdout of the child process, execv it.
         * the parent should sometimes wait and sometimes not wait for
	 * the child process (you must figure out when). if foreground
	 * is true then basically you should wait but when we are
	 * running a command in a pipe such as PROG1 | PROG2 you might
	 * not want to wait for each of PROG1 and PROG2...
	 * 
	 * hints:
	 *  snprintf is useful for constructing strings.
	 *  access is useful for checking wether a path refers to an 
	 *      executable program.
	 * 
	 * 
	 */

	char *program_name, *filepath, filename[100], oldpwd[100];
	unsigned list_size;
	pid_t pid;

	program_name = argv[0];

//	printf("%s Input fd: %d, Output fd: %d\n", __func__, dup(0), dup(1));

	if ( ! strncmp(program_name, "cd", 3) ) {
		/* This will be a unique case, since cd is no binary (shell built in) */
		memset(oldpwd, '\0', sizeof oldpwd);

		if ( argc == 1 ){
			snprintf(oldpwd, sizeof oldpwd, "%s", getenv("PWD"));
			char * home = getenv("HOME");
			chdir(home);
			setenv("PWD", home, 1);
			setenv("OLDPWD", oldpwd, 1);
		} else if ( argc > 1 && strncmp(argv[1], "-", 2) == 0) {
			char *pwd = getenv("PWD");
			snprintf(oldpwd, sizeof oldpwd, "%s", getenv("OLDPWD"));
	//		printf("Attempting to change to: %s\n", oldpwd);
			if (chdir(oldpwd)) {
				error("Could not change to old dir.\n");
			}
			setenv("OLDPWD", pwd, 1);
			setenv("PWD", oldpwd, 1);
			printf("%s\n", oldpwd);
		} else if ( argc > 1 && chdir(argv[1]) == 0) {
			setenv("OLDPWD", getenv("PWD"), 1);
			char pwd_string[MAX_PATH_LENGTH];
			memset(pwd_string, 0, sizeof pwd_string);
			getwd(pwd_string);
			setenv("PWD", pwd_string, 1);
		} else {
			error("Not a valid path.\n");
		}
	//	printf("pwd: %s\noldpwd: %s\n", getenv("PWD"), getenv("OLDPWD"));


	} else {

		list_size = length(path_dir_list);

		unsigned i = 0;
		memset(filename, '\0', 100);
		bool program_found = false;
		for (; i < list_size; ++i){
			filepath = (char*)path_dir_list->data;

			snprintf(filename, sizeof(filename)-1, "%s/%s", filepath, program_name);

			if ( ! access(filename, X_OK) ) {
				/* We can access the file */
				program_found = true;
				pid = fork();
				if ( pid == 0 ){
					// We are the child process
					execv(filename, argv);

				} else {
					// We are the parent process

					if ( doing_pipe ) {
						//do nothing
					} else if ( !foreground ) {
						//do nothing, only when foreground and not pipe, or after the last pipe.
						printf("[%zu]\n", pid);
					} else if ( foreground ){
						int status;
						waitpid(pid, &status, 0);
					} 
				}

			}

			path_dir_list = path_dir_list->succ;
		}
		if (!program_found) {
			error("Program not found.");
		}

	}

}

void parse_line(void)
{
	char*		argv[MAX_ARG + 1];
	int		argc;
	int		pipe_fd[2];	/* 1 for producer and 0 for consumer. */
	int old_std_out = dup(1);
	int old_std_in = dup(0);
	token_type_t	type;
	bool		foreground;
	bool		doing_pipe;

	input_fd	= 0;
	output_fd	= 0;
	argc		= 0;

	for (;;) {
			
		foreground	= true;
		doing_pipe	= false;

		type = gettoken(&argv[argc]);

		switch (type) {
		case NORMAL:
			argc += 1;
			break;

		case INPUT:
			type = gettoken(&argv[argc]);
			if (type != NORMAL) {
				error("expected file name: but found %s", 
					argv[argc]);
				return;
			}

			input_fd = open(argv[argc], O_RDONLY);

			if (input_fd < 0) {
				error("cannot read from %s", argv[argc]);
				return;
			} else {
				dup2(input_fd, 0);

			}
			break;

		case OUTPUT:
			type = gettoken(&argv[argc]);
			if (type != NORMAL) {
				error("expected file name: but found %s", 
					argv[argc]);
				return;
			}

			output_fd = open(argv[argc], O_CREAT | O_WRONLY, PERM);

			if (output_fd < 0)
				error("cannot write to %s", argv[argc]);

			//Redirect output
			dup2(output_fd, 1);
			break;

		case PIPE:
			doing_pipe = true;
			
			pipe(pipe_fd);
			//redirect output to pipe
			dup2(pipe_fd[1], 1);

			/*FALLTHROUGH*/

		case AMPERSAND:
			foreground = false;

			/*FALLTHROUGH*/

		case NEWLINE:
		case SEMICOLON:

			if (argc == 0)
				return;
						
			argv[argc] = NULL;

			//printf("%s\n", "Running program now.");
			run_program(argv, argc, foreground, doing_pipe);

			if (doing_pipe) {
				//redirect input to pipe
				close(pipe_fd[1]);
				dup2(pipe_fd[0], 0);
				//reset output to stdout
				dup2(old_std_out,1);
				doing_pipe = false;
			} else {
				//reset input to stdin
				
				dup2(old_std_in, 0);
			}

			if (output_fd != 0) {
				close(output_fd);
				//Change back output to terminal.
				dup2(old_std_out, 1);
			}

			if (input_fd != 0) {
				close(input_fd);
				//Change back input to keyboard.
				dup2(old_std_in, 0);
			}

			input_fd	= 0;
			output_fd	= 0;
			argc		= 0;

			if (type == NEWLINE)
				return;

			break;
		}
	}
}

/* init_search_path: make a list of directories to look for programs in. */
static void init_search_path(void)
{
	char*		dir_start;
	char*		path;
	char*		s;
	list_t*		p;
	bool		proceed;

	path = getenv("PATH");

	/* path may look like "/bin:/usr/bin:/usr/local/bin" 
	 * and this function makes a list with strings 
	 * "/bin" "usr/bin" "usr/local/bin"
 	 *
	 */

	dir_start = malloc(1+strlen(path));
	if (dir_start == NULL) {
		error("out of memory.");
		exit(1);
	}

	strcpy(dir_start, path);

	path_dir_list = NULL;

	if (path == NULL || *path == 0) {
		path_dir_list = new_list("");
		return;
	}

	proceed = true;

	while (proceed) {
		s = dir_start;
		while (*s != ':' && *s != 0)
			s++;
		if (*s == ':')
			*s = 0;
		else
			proceed = false;

		insert_last(&path_dir_list, dir_start);

		dir_start = s + 1;
	}

	p = path_dir_list;

	if (p == NULL)
		return;

#if 0
	do {
		printf("%s\n", (char*)p->data);
		p = p->succ;	
	} while (p != path_dir_list);
#endif
}

/* main: main program of simple shell. */
int main(int argc, char** argv)
{
	progname = argv[0];

	init_search_path();	

	while (fetch_line("% ") != EOF)
		parse_line();

	return 0;
}
