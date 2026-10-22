/* SPDX-License-Identifier: GPL-2.0-only */
/* Run shell commands without blocking PID 1 on USB or child processes. */
static long shell_pid = -1;
static int shell_started, shell_running, shell_capture, shell_status;

static void shell_reap(void)
{
	unsigned int budget = 16;
	int status;
	long pid;

	if (!shell_started)
		return;
	while (budget--) {
		pid = syscall5(114, -1, (long)&status, 1, 0, 0); /* wait4(WNOHANG) */
		if (pid <= 0) {
			if (pid == -10 && !shell_running) /* ECHILD */
				shell_started = 0;
			return;
		}
		if (pid == shell_pid) {
			shell_status = status;
			shell_running = 0;
			shell_pid = -1;
		}
	}
}

static void shell_start(const char *command)
{
	const char *argv[] = { "sh", "-c", command, 0 };
	const char *env[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
		"HOME=/", "TERM=linux", "LC_ALL=C", 0 };
	int pipes[2];
	long pid, fd;

	if (shell_capture) {
		usb_reply("\n[WT6580 shell busy]\n");
		return;
	}
	if (syscall5(359, (long)pipes, 02000000, 0, 0, 0) < 0) { /* pipe2(CLOEXEC) */
		usb_reply("\n[WT6580 shell pipe failed]\n");
		return;
	}
	if (syscall5(55, pipes[0], 4, O_NONBLOCK, 0, 0) < 0) { /* fcntl(F_SETFL) */
		syscall5(6, pipes[0], 0, 0, 0, 0);
		syscall5(6, pipes[1], 0, 0, 0, 0);
		usb_reply("\n[WT6580 shell nonblocking setup failed]\n");
		return;
	}
	pid = syscall5(2, 0, 0, 0, 0, 0); /* fork */
	if (!pid) {
		/* Children get blocking pipe output and no access to the CDC input. */
		syscall5(66, 0, 0, 0, 0, 0); /* setsid */
		fd = open_file("/dev/null", 0);
		if (fd < 0 || syscall5(63, fd, 0, 0, 0, 0) < 0 ||
		    syscall5(63, pipes[1], 1, 0, 0, 0) < 0 ||
		    syscall5(63, pipes[1], 2, 0, 0, 0) < 0)
			goto failed;
		/* Drop inherited watchdog, DRM and CDC descriptors after dup2. */
		syscall5(436, 3, ~0U, 0, 0, 0); /* close_range */
		syscall5(11, (long)"/bin/sh", (long)argv, (long)env, 0, 0);
		log_message(2, "WT6580: /bin/sh exec failed; include a static BusyBox\n");
failed:
		syscall5(1, 127, 0, 0, 0, 0);
		for (;;)
			;
	}
	syscall5(6, pipes[1], 0, 0, 0, 0);
	if (pid < 0) {
		syscall5(6, pipes[0], 0, 0, 0, 0);
		usb_reply("\n[WT6580 shell fork failed]\n");
		return;
	}
	dump_fd = pipes[0];
	shell_pid = pid;
	shell_started = shell_running = shell_capture = 1;
	shell_status = 0;
}

static void shell_result(void)
{
	char reply[80], digits[16];
	const char *prefix = "\n[WT6580 shell exit=";
	unsigned int n = 0, d = 0;
	unsigned int code = shell_status & 127 ?
		128 + (shell_status & 127) : (shell_status >> 8) & 255;

	while (*prefix)
		reply[n++] = *prefix++;
	do {
		digits[d++] = '0' + code % 10;
		code /= 10;
	} while (code);
	while (d)
		reply[n++] = digits[--d];
	reply[n++] = ']';
	reply[n++] = '\n';
	reply[n] = 0;
	usb_reply(reply);
	shell_capture = 0;
}
