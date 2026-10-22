// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the PID 1 USB protocol without touching a host device or watchdog. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

static long simulated_syscall(long nr, long a, long b, long c, long d, long e);
#define WT6580_INIT_TEST simulated_syscall
#include "init.c"

static const char *input;
static size_t input_pos;
static char output[32768];
static size_t output_len;
static char dump[10000];
static size_t dump_pos;
static int blocked_writes;
static int disconnect_write;
static int overwritten_record;
static int opens;
static int dumps;
static long read_limit;
static long write_limit;
static int syncs;
static int unmounts;
static int reboots;
static int prepare_failure;
static int marker_verified;
static int missing_bootmode;
static int hangup_read;
static int closes;
static int pipe_eof;

static long simulated_syscall(long nr, long a, long b, long c, long d, long e)
{
	(void)d;
	(void)e;
	switch (nr) {
	case 5:
		if (!strcmp((const char *)a,
			    "/sys/bus/platform/devices/1000f000.reboot-mode/bootloader")) {
			assert(b == 1);
			return missing_bootmode ? -2 : 13;
		}
		assert(b & O_NONBLOCK);
		if (!strcmp((const char *)a, "/dev/ttyGS0")) {
			opens++;
			return 11;
		}
		assert(!strcmp((const char *)a, "/dev/kmsg"));
		dumps++;
		dump_pos = 0;
		return 12;
	case 3:
		if (a == 11) {
			if (hangup_read && input_pos == 3) {
				hangup_read = 0;
				return 0; /* Hung-up ttyGS0 reports EOF until closed. */
			}
			if (!input[input_pos])
				return -11;
			assert(c == 1);
			*(char *)b = input[input_pos++];
			return 1;
		}
		assert(a == 12);
		if (overwritten_record) {
			overwritten_record = 0;
			return -32;
		}
		if (dump_pos == sizeof(dump))
			return pipe_eof ? 0 : -11;
		if (c > (long)(sizeof(dump) - dump_pos))
			c = sizeof(dump) - dump_pos;
		if (c > read_limit)
			c = read_limit;
		memcpy((void *)b, dump + dump_pos, c);
		dump_pos += c;
		return c;
	case 4:
		if (a == 13) {
			assert(c == 1 && *(char *)b == '1');
			if (prepare_failure)
				return -5;
			marker_verified++;
			return 1;
		}
		assert(a == 11);
		if (blocked_writes) {
			blocked_writes--;
			return -11;
		}
		if (disconnect_write) {
			disconnect_write = 0;
			return -5;
		}
		if (c > write_limit)
			c = write_limit; /* Short writes must preserve pending bytes. */
		assert(output_len + c < sizeof(output));
		memcpy(output + output_len, (const void *)b, c);
		output_len += c;
		output[output_len] = 0;
		return c;
	case 6:
		if (a == 11)
			closes++;
		return 0;
	case 54:
		return -25;
	case 52:
		assert(!strncmp((const char *)a, "/mnt/", 5) && !b);
		unmounts++;
		return -2; /* Also exercise reboot when filesystems are not mounted. */
	case 36:
		syncs++;
		return 0;
	case 88:
		assert(syncs == 1 && unmounts == 4);
		assert((unsigned long)a == 0xfee1deadUL);
		assert(b == 672274793);
		if ((unsigned long)c == 0xa1b2c3d4UL) {
			assert(marker_verified == 1);
			assert(!strcmp((const char *)d, "bootloader"));
		} else {
			assert(c == 0x01234567 && d == 0 && !marker_verified);
		}
		reboots++;
		return -1; /* Exercise failure reporting without rebooting the host. */
	default:
		/* Protocol polling must not sleep, reboot or feed a real watchdog. */
		assert(0);
		return -1;
	}
}

static void reset_fixture(const char *commands)
{
	input = commands;
	input_pos = output_len = dump_pos = 0;
	output[0] = 0;
	blocked_writes = disconnect_write = overwritten_record = 0;
	opens = dumps = 0;
	syncs = reboots = unmounts = 0;
	prepare_failure = marker_verified = missing_bootmode = 0;
	hangup_read = closes = pipe_eof = 0;
	shell_started = shell_running = shell_capture = shell_status = 0;
	shell_pid = -1;
	read_limit = sizeof(dump);
	write_limit = 7;
	usb_fd = dump_fd = -1;
	usb_output_len = usb_output_pos = 0;
	usb_command_len = 0;
	usb_command_overflow = 0;
}

static void run_polls(void)
{
	unsigned int i;

	for (i = 0; i < 10000; i++)
		usb_poll_console();
	assert(!input[input_pos]);
	assert(dump_fd < 0);
	assert(usb_output_len == 0);
}

int main(void)
{
	char overflow[sizeof(usb_command) + 20];
	unsigned int steps;

	reset_fixture("help\r\nhelp\n");
	blocked_writes = 20;
	run_polls();
	assert(strstr(output, "help, dmesg, status"));
	assert(strstr(strstr(output, "WT6580") + 1, "WT6580"));

	memset(dump, 'D', sizeof(dump));
	reset_fixture("dmesg\n");
	disconnect_write = 1;
	overwritten_record = 1;
	run_polls();
	assert(opens == 2 && dumps == 1);
	assert(!memcmp(output, dump, sizeof(dump)));
	assert(strstr(output + sizeof(dump), "dump complete"));

	/* /dev/kmsg supplies one record per read. Drain 500 small records
	 * without requiring a sleep between each read and write.
	 */
	reset_fixture("dmesg\n");
	read_limit = 20;
	write_limit = sizeof(usb_output);
	for (steps = 0; steps < 1100 && usb_poll_console(); steps++)
		;
	assert(steps < 1100 && dump_fd < 0);
	assert(!memcmp(output, dump, sizeof(dump)));
	assert(strstr(output + sizeof(dump), "dump complete"));

	reset_fixture("help\n");
	assert(usb_poll_console());
	blocked_writes = 1;
	assert(!usb_poll_console()); /* Backpressure must yield to the main loop. */
	run_polls();

	memset(overflow, 'a', sizeof(overflow));
	overflow[sizeof(overflow) - 2] = '\n';
	overflow[sizeof(overflow) - 1] = 0;
	reset_fixture(overflow);
	run_polls();
	assert(strstr(output, "command too long") && !dumps);

	/* An empty child pipe is not EOF; keep it open until the shell exits. */
	reset_fixture("");
	dump_fd = 12;
	dump_pos = sizeof(dump);
	shell_capture = shell_running = 1;
	assert(!usb_poll_console() && dump_fd == 12 && !output_len);
	shell_running = 0;
	shell_status = 7 << 8;
	pipe_eof = 1;
	run_polls();
	assert(strstr(output, "shell exit=7") && !strstr(output, "dump complete"));

	/* Stream a child's output through short USB writes, then report exit. */
	reset_fixture("");
	dump_fd = 12;
	shell_capture = pipe_eof = 1;
	run_polls();
	assert(!memcmp(output, dump, sizeof(dump)));
	assert(strstr(output + sizeof(dump), "shell exit=0"));

	reset_fixture("dmesX\bg\n");
	run_polls();
	assert(dumps == 1);

	reset_fixture("rebootx\n");
	run_polls();
	assert(!syncs && !reboots);
	reset_fixture("reboot\r\n");
	run_polls();
	assert(syncs == 1 && reboots == 1);
	assert(strstr(output, "rebooting") && strstr(output, "reboot failed"));
	reset_fixture("reboot bootloader\r\n");
	run_polls();
	assert(syncs == 1 && reboots == 1 && marker_verified == 1);
	assert(strstr(output, "rebooting to bootloader"));
	reset_fixture("reboot bootloader\n");
	prepare_failure = 1;
	run_polls();
	assert(!reboots && strstr(output, "reboot cancelled"));
	reset_fixture("reboot bootloader\n");
	missing_bootmode = 1;
	run_polls();
	assert(!reboots && strstr(output, "driver unavailable"));
	reset_fixture("reboot bootloaderx\n");
	run_polls();
	assert(!syncs && !reboots && strstr(output, "unknown command"));

	reset_fixture("helhelp\n");
	hangup_read = 1;
	run_polls();
	assert(opens == 2 && closes == 1);
	assert(strstr(output, "help, dmesg, status"));
	assert(!strstr(output, "unknown command"));
	puts("USB console: backpressure, short writes, reconnect, kmsg overrun, "
	     "queued commands, fast record drain, yielding and input bounds passed");
	return 0;
}
