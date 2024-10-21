// SPDX-License-Identifier: BSD-3-Clause
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include "cmd.h"
#include "utils.h"

#define READ		0
#define WRITE		1
size_t strscpy(char *dest, const char *src, size_t count)
{
	size_t len = 0;

	while (count > 1 && *src) {
		*dest++ = *src++;
		len++;
		count--;
	}

	*dest = '\0';

	return len;
}

void terminate_all_processes(void)
{
	pid_t pgid = getpgrp();

	kill(-pgid, SIGTERM);
}

static bool shell_cd(word_t *dir)
{
	if (dir != NULL) {
		printf("%s\n", dir->string);
		if (chdir(dir->string) != 0)
			return 1;
	}
	return 0;
}

static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	if (strcmp(s->verb->string, "exit") == 0 || strcmp(s->verb->string, "EXIT") == 0
		|| strcmp(s->verb->string, "quit") == 0) {
		terminate_all_processes();
		return SHELL_EXIT;
	}
	if (strcmp(s->verb->string, "cd") == 0) {
		if (s->out != NULL) {
			int fd;

			if (s->io_flags == 0 && s->err == NULL)
				fd = open(s->out->string, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			else
				fd = open(s->out->string, O_WRONLY | O_CREAT | O_APPEND, 0666);
			if (fd == -1)
				return SHELL_EXIT;
			close(fd);
		}
		return shell_cd(s->params);
	}
	int size;
	char **argv = get_argv(s, &size);
	int status;
	int pid = fork();

	if (pid == 0) {
		if (s->in != NULL) {
			int fd = open(s->in->string, O_RDONLY);

			if (fd != -1) {
				dup2(fd, STDIN_FILENO);
				close(fd);
			} else {
				close(fd);
			}
		}
		if (s->out != NULL) {
			int fd;

			if (strcmp(s->out->string, "0") == 0)
				strcat(s->out->string, ".txt");
			if (s->io_flags == 0 && s->err == NULL)
				fd = open(s->out->string, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			else
				fd = open(s->out->string, O_WRONLY | O_CREAT | O_APPEND, 0666);
			if (fd == -1) {
				close(fd);
			} else {
				dup2(fd, STDOUT_FILENO);
				close(fd);
			}
		}
		if (s->err != NULL) {
			int fd;

			if (s->io_flags == 0)
				fd = open(s->err->string, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			else
				fd = open(s->err->string, O_WRONLY | O_CREAT | O_APPEND, 0666);
			if (fd == -1)
				return SHELL_EXIT;
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		int found = 0;

		if (size == 1) {
			char s[100];

			strscpy(s, argv[0], sizeof(s)-1);
			for (int i = 0; i < strlen(s); i++) {
				if (s[i] == '=') {
					found = 1;
					break;
				}
			}
			if (found) {
				char *saveptr;
				char *p = strtok_r(s, "=", &saveptr);
				char s1[100], s2[100];

				if (p != NULL) {
					strscpy(s1, p, sizeof(s1)-1);
					p = strtok_r(NULL, "=", &saveptr);
					if (p != NULL) {
						strscpy(s2, p, sizeof(s2)-1);
						setenv(s1, s2, 1);
					}
				}
			}
		}
			if (execvp(argv[0], argv) == -1 && found == 0) {
				printf("Execution failed for '%s'\n", argv[0]);
				return 1;
			}
	} else {
		waitpid(pid, &status, 0);
	}
	return WEXITSTATUS(status);
}

static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	int pid = fork();

	if (pid == 0) {
		parse_command(cmd1, level+1, father);
		exit(0);
	}
	int pid2 = fork();

	if (pid2 == 0) {
		parse_command(cmd2, level+1, father);
		exit(0);
	}
	int status1 = 0;
	int status2 = 0;

	waitpid(pid, &status1, 0);
	waitpid(pid2, &status2, 0);
	return status1 && status2;
}

static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level, command_t *father)
{
	int pipes[2];

	pipe(pipes);
	int x, y;
	int pid = fork();

	if (pid == 0) {
		close(pipes[0]);
		dup2(pipes[1], STDOUT_FILENO);
		close(pipes[1]);
		x = parse_command(cmd1, level + 1, father);
		exit(x);
	} else if (pid > 0) {
		int status2 = 0;

		close(pipes[1]);
		int pid2  = fork();

		if (pid2 == 0) {
			dup2(pipes[0], STDIN_FILENO);
			y = parse_command(cmd2, level+1, father);
			exit(y);
		}
		close(pipes[0]);
		waitpid(pid2, &status2, 0);

		return WEXITSTATUS(status2);
	}
	return true;
}

static int run_seq(command_t *cmd, int level, command_t *father)
{
	static int x;

	if (cmd->scmd != NULL && cmd != NULL && cmd->cmd1 == NULL && cmd->cmd2 == NULL)
		x = parse_command(cmd, level, father);
	if (cmd->op == OP_PARALLEL) {
		x = run_in_parallel(cmd->cmd1, cmd->cmd2, level, father);
	} else if (cmd->op == OP_PIPE) {
		x  = run_on_pipe(cmd->cmd1, cmd->cmd2, level, father);
	} else if (cmd->op == OP_SEQUENTIAL) {
		run_seq(cmd->cmd1, level, father);
		run_seq(cmd->cmd2, level, father);
	} else if (cmd->op == OP_CONDITIONAL_ZERO) {
		if (cmd->cmd1->scmd != NULL)
			x = parse_command(cmd->cmd1, level, father);
		else
			run_seq(cmd->cmd1, level, father);
		if (x == 0) {
			if (cmd->cmd2->scmd != NULL)
				x = parse_command(cmd->cmd2, level, father);
			else
				run_seq(cmd->cmd2, level, father);
		}
	} else if (cmd->op == OP_CONDITIONAL_NZERO) {
		if (cmd->cmd1->scmd != NULL)
			x  = parse_command(cmd->cmd1, level, father);
		else
			run_seq(cmd->cmd1, level, father);
		if (x != 0) {
			if (cmd->cmd2->scmd != NULL)
				x = parse_command(cmd->cmd2, level, father);
			else
				run_seq(cmd->cmd2, level, father);
		}
	}
	return -3;
}

int parse_command(command_t *c, int level, command_t *father)
{
	if (c->op == OP_NONE)
		return parse_simple(c->scmd, level, father);
	int ign = 0;

	switch (c->op) {
	case OP_SEQUENTIAL:
		run_seq(c->cmd1, level, father);
		run_seq(c->cmd2, level, father);
		break;
	case OP_PARALLEL:
		return run_in_parallel(c->cmd1, c->cmd2, level, father);
	case OP_CONDITIONAL_NZERO:
		ign = 1;
		int x = parse_command(c->cmd1, level, father);

		if (x != 0)
			x = parse_command(c->cmd2, level, father);
		return x;
	case OP_CONDITIONAL_ZERO:
		ign = 1;
		if (ign == 1)
			ign = 0;
		int x2 = 0;

		x2 = parse_command(c->cmd1, level, father);
		if (x2 == 0)
			x2 = parse_command(c->cmd2, level, father);
		return x2;
	case OP_PIPE:
		if (c->cmd1->scmd != NULL || c->cmd2->scmd != NULL)
			return run_on_pipe(c->cmd1, c->cmd2, level, father);
		parse_command(c->cmd1, level, father);
		parse_command(c->cmd2, level, father);
		break;
	default:
		return SHELL_EXIT;
	}
	return 0;
}

