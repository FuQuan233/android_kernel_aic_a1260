// SPDX-License-Identifier: GPL-2.0-only
/* Exercise real fork/pipe/exec, idle output, backpressure and orphan reaping. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static long dump_fd = -1;
static char reply[256];
static int fail_pipe, fail_fork;
static void usb_reply(const char *s) { assert(strlen(s) < sizeof(reply)); strcpy(reply, s); }
static long open_file(const char *path, long flags) { int r = open(path, flags); return r < 0 ? -errno : r; }
static void log_message(long fd, const char *s) { assert(write(fd, s, strlen(s)) >= 0); }
static long syscall5(long nr, long a, long b, long c, long d, long e)
{
	long r;
	(void)d; (void)e;
	switch (nr) {
	case 359: if (fail_pipe) return -EMFILE; r = pipe2((int *)a, b); break;
	case 55: r = fcntl(a, b, c); break;
	case 2: if (fail_fork) return -EAGAIN; r = fork(); break;
	case 6: r = close(a); break;
	case 66: r = setsid(); break;
	case 63: r = dup2(a, b); break;
	case 436: r = syscall(SYS_close_range, (unsigned int)a, (unsigned int)b, c); break;
	case 11: r = execve((char *)a, (char **)b, (char **)c); break;
	case 1: _exit(a);
	case 114: r = waitpid(a, (int *)b, c); break;
	default: abort();
	}
	return r < 0 ? -errno : r;
}
#include "shell.h"

static size_t collect(char *output, size_t capacity, int *idle)
{
	size_t used = 0;
	unsigned int loops;
	int eof = 0;

	for (loops = 0; loops < 5000 && (!eof || shell_running); loops++) {
		char buf[73];
		ssize_t n;
		shell_reap();
		if (!eof) {
			n = read(dump_fd, buf, sizeof(buf));
			if (n > 0) {
				assert(used + n < capacity);
				memcpy(output + used, buf, n);
				used += n;
			} else if (!n) {
				eof = 1;
				close(dump_fd);
				dump_fd = -1;
			} else {
				assert(errno == EAGAIN);
				(*idle)++;
			}
		}
		usleep(1000);
	}
	assert(eof && !shell_running && shell_capture);
	output[used] = 0;
	shell_result();
	assert(!shell_capture);
	return used;
}

int main(void)
{
	char *output = malloc(70000);
	int idle = 0;
	assert(output);
	assert(!prctl(PR_SET_CHILD_SUBREAPER, 1));
	fail_pipe = 1;
	shell_start("true");
	assert(strstr(reply, "pipe failed") && !shell_capture);
	fail_pipe = 0; fail_fork = 1;
	shell_start("true");
	assert(strstr(reply, "fork failed") && !shell_capture);
	fail_fork = 0;

	shell_start("printf stdout; printf stderr >&2; exit 7");
	collect(output, 70000, &idle);
	assert(!strcmp(output, "stdoutstderr") && strstr(reply, "exit=7"));
	shell_start("sleep .05; printf done");
	collect(output, 70000, &idle);
	assert(!strcmp(output, "done") && strstr(reply, "exit=0") && idle);
	shell_start("head -c 65536 /dev/zero");
	usleep(100000); /* Make the child block on a full output pipe. */
	assert(shell_capture && shell_running);
	assert(collect(output, 70000, &idle) == 65536 && strstr(reply, "exit=0"));
	shell_start("sleep .03 & printf background");
	collect(output, 70000, &idle);
	assert(!strcmp(output, "background") && strstr(reply, "exit=0"));
	shell_reap();
	assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
	free(output);
	puts("Shell: fork errors, idle pipes, stdout/stderr, exit codes, backpressure and orphan reaping passed");
	return 0;
}
