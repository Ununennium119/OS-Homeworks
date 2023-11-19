#include "process.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <termios.h>

#include "shell.h"

/**
 * Executes the process p.
 * If the shell is in interactive mode and the process is a foreground process,
 * then p should take control of the terminal.
 */
void launch_process(process *p) {
    // redirect input and output
    if (p->stdout != STDOUT_FILENO) {
        if (dup2(p->stdout, STDOUT_FILENO) == -1) {
            perror("Failed to change output");
            exit(1);
        }
    }
    if (p->stdin != STDIN_FILENO) {
        if (dup2(p->stdin, STDIN_FILENO) == -1) {
            perror("Failed to change input");
            exit(1);
        }
    }

    // execute command
    if (access(p->argv[0], F_OK) == 0) {
        if (execv(p->argv[0], p->argv) == -1) {
            perror("Failed to execute command");
            exit(1);
        }
    } else {
        perror("Failed to execute command");
        exit(1);
    }
}

/* Put a process in the foreground. This function assumes that the shell
 * is in interactive mode. If the cont argument is true, send the process
 * group a SIGCONT signal to wake it up.
 */
void put_process_in_foreground(process *p, int cont) {
    tcsetpgrp(shell_terminal, p->pid);
	
    if (cont) {
        if (kill(-p->pid, SIGCONT) < 0) perror("kill (SIGCONT)");
    }

    if (waitpid(p->pid, &p->status, WUNTRACED) == -1) {
        perror("Failed to wait for child process.");
        exit(1);
    }

    tcsetpgrp(shell_terminal, shell_pgid);
}

/* Put a process in the background. If the cont argument is true, send
 * the process group a SIGCONT signal to wake it up. */
void put_process_in_background(process *p, int cont) {
    // do nothing
}

void free_process(process *p) {
    free(p->argv);
    free(p);
}

char is_process_background(int pid) {
    process *current_process = first_process;
    while (current_process != NULL) {
        if (current_process->pid == pid) {
            return current_process->background;
        }
        current_process = current_process->next;
    }
    return 0;
}
