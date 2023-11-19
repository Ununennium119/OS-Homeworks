#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define FALSE 0
#define TRUE 1
#define INPUT_STRING_SIZE 80
#define DEFAULT 0
#define IGNORE 1

#include "io.h"
#include "parse.h"
#include "process.h"
#include "shell.h"

int cmd_quit(tok_t arg[]);

int cmd_help(tok_t arg[]);

int cmd_pwd(tok_t arg[]);

int cmd_cd(tok_t arg[]);

int cmd_wait(tok_t arg[]);

void change_signal_handle_mode(char ignore);

/* Command Lookup table */
typedef int cmd_fun_t(tok_t args[]); /* cmd functions take token array and return int */

typedef struct fun_desc {
    cmd_fun_t *fun;
    char *cmd;
    char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_quit, "quit", "quit the command shell"},
    {cmd_pwd, "pwd", "print current working directory"},
    {cmd_cd, "cd", "change current directory"},
    {cmd_wait, "wait", "wait for all background processes"},
};

int cmd_quit(tok_t arg[]) {
    printf("Bye\n");
    fflush(stdout);
    exit(0);
}

int cmd_help(tok_t arg[]) {
    int i;
    for (i = 0; i < (sizeof(cmd_table) / sizeof(fun_desc_t)); i++) {
        printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
    }
    return 1;
}

int cmd_pwd(tok_t arg[]) {
    int buffer_size = 1024;
    char *buffer = (char *)malloc(buffer_size * sizeof(char));
    if (buffer == NULL) {
        perror("Could not get working directory");
        return -1;
    }
    char *cwd = getcwd(buffer, buffer_size);
    if (cwd != NULL) {
        printf("%s\n", cwd);
        return 1;
    } else {
        perror("Could not get working directory");
        return -1;
    }
}

int cmd_cd(tok_t arg[]) {
    tok_t path = arg[0];
    if (chdir(path) != 0) {
        perror("Failed to changed directory");
        return -1;
    }
    return 1;
}

int cmd_wait(tok_t arg[]) {
    while (waitpid(WAIT_ANY, NULL, 0) > 0) {}
    return 1;
}

int lookup(char cmd[]) {
    int i;
    for (i = 0; i < (sizeof(cmd_table) / sizeof(fun_desc_t)); i++) {
        if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0)) return i;
    }
    return -1;
}

void change_signal_handle_mode(char ignore) {
    __sighandler_t mode = ignore ? SIG_IGN : SIG_DFL;
    signal(SIGINT, mode);
    signal(SIGQUIT, mode);
    signal(SIGTSTP, mode);
    signal(SIGTTIN, mode);
    signal(SIGTTOU, mode);
}

void init_shell() {
    /* Check if we are running interactively */
    shell_terminal = STDIN_FILENO;

    /** Note that we cannot take control of the terminal if the shell is not
     * interactive */
    shell_is_interactive = isatty(shell_terminal);

    if (shell_is_interactive) {
        /* force into foreground */
        while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
            kill(-shell_pgid, SIGTTIN);

        shell_pgid = getpid();
        /* Put shell in its own process group */
        if (setpgid(shell_pgid, shell_pgid) < 0) {
            perror("Couldn't put the shell in its own process group");
            exit(1);
        }

        /* Take control of the terminal */
        tcsetpgrp(shell_terminal, shell_pgid);
        tcgetattr(shell_terminal, &shell_tmodes);

        /* ignore signals */
        change_signal_handle_mode(IGNORE);
    }
}

/**
 * Add a process to our process list
 */
void add_process(process *p) {
	if (first_process == NULL) {
		first_process = p;
		return;
	}

	process *current_p = first_process;
	while (current_p->next != NULL) {
		current_p = current_p->next;
	}
	p->prev = current_p;
	current_p->next = p;
}

/**
 * Creates a process given the inputString from stdin
 */
process *create_process(tok_t *t) {
    process *p = (process *)malloc(sizeof(process));

    // initialize process state
    p->completed = 0;
    p->stopped = 0;
    p->background = 0;
    p->tmodes = shell_tmodes;

    // extract command's args and input/output file path
    p->argc = 0;
    p->argv = (char **)malloc(MAXTOKS * sizeof(char *));
    char *output_file_path = NULL;
    char *input_file_path = NULL;
    for (int i = 0; t[i] != NULL; i++) {
        char *current_token = t[i];
        if (strcmp(current_token, ">") == 0) {
            i++;
            output_file_path = t[i];
            if (output_file_path == NULL) {
                printf("Invalid command.\n");
                free_process(p);
                return NULL;
            }
        } else if (strcmp(current_token, "<") == 0) {
            i++;
            input_file_path = t[i];
            if (input_file_path == NULL) {
                printf("Invalid command.\n");
                free_process(p);
                return NULL;
            }
        } else if (strcmp(current_token, "&") == 0) {
            p->background = 1;
        } else {
            p->argv[p->argc] = current_token;
            p->argc++;
        }
    }
    freeToks(t);

    // get output and input file descriptors
    p->stdout = STDOUT_FILENO;
    p->stdin = STDIN_FILENO;
    p->stderr = STDERR_FILENO;
    if (output_file_path != NULL) {
        p->stdout = open(output_file_path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
        if (p->stdout == -1) {
            perror("Failed to open output file");
            free_process(p);
            return NULL;
        }
    }
    if (input_file_path != NULL) {
        p->stdin = open(input_file_path, O_RDONLY);
        if (p->stdin == -1) {
            perror("Failed to open input file");
            free_process(p);
            return NULL;
        }
    }

    return p;
}

int shell(int argc, char *argv[]) {
    char *s; /* user input string */
    int fundex = -1;
    pid_t cpid;

    init_shell();

    while ((s = freadln(stdin))) {        
        // run command
        tok_t *t = getToks(s);	/* break the line into tokens */
        fundex = lookup(t[0]);	/* Is first argv a shell literal */
        if (fundex >= 0) {
            cmd_table[fundex].fun(&t[1]);	/* run internal command */
        } else {
            /* run external command */
            // create and add process
            process *p = create_process(t);
            if (p == NULL) continue;
			add_process(p);

            // search PATH for command
            char *path = (char *)malloc(1024 * sizeof(char));
            strcpy(path, getenv("PATH"));
            char *current_path = strtok(path, ":");
            char command_path[1024];
            while (current_path != NULL) {
                strcpy(command_path, current_path);
                strcat(command_path, "/");
                strcat(command_path, p->argv[0]);
                if (access(command_path, F_OK) == 0) {
                    p->argv[0] = command_path;
                    break;
                }
                current_path = strtok(NULL, ":");
            }
            free(path);

            // wait for background processes
            waitpid(-1, NULL, WNOHANG);

            // run process
            cpid = fork();
            if (cpid == -1) {
                perror("Failed to execute command");
            } else if (cpid == 0) {
                p->pid = getpid();
                change_signal_handle_mode(DEFAULT);
                launch_process(p);
            } else {
                p->pid = cpid;
                if (!p->background) {
                    setpgid(p->pid, p->pid);
                    put_process_in_foreground(p, 0);
                }
            }
            p->completed = 1;
            
            // close output/input files
            if (p->stdout != STDOUT_FILENO) {
                if (close(p->stdout) == -1) {
                    perror("Failed to close output file");
                }
            }
            if (p->stdin != STDIN_FILENO) {
                if (close(p->stdin) == -1) {
                    perror("Failed to close input file");
                }
            }
        }
    }
    return 0;
}
