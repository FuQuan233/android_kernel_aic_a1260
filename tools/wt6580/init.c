// SPDX-License-Identifier: GPL-2.0-only
/* ARM EABI diagnostic PID 1 with a nonblocking USB CDC debug interface. */

#ifndef WT6580_INIT_TEST
/* Compiler-generated structure copies still need these in a libc-free init. */
void *memcpy(void *dst, const void *src, __SIZE_TYPE__ size)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (size--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int value, __SIZE_TYPE__ size)
{
	unsigned char *d = dst;

	while (size--)
		*d++ = value;
	return dst;
}
#endif

#define O_NONBLOCK 0x800
#define O_NOCTTY 0x100

static long syscall5(long nr, long a, long b, long c, long d, long e)
{
#ifdef WT6580_INIT_TEST
	return WT6580_INIT_TEST(nr, a, b, c, d, e);
#else
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r7 __asm__("r7") = nr;

	__asm__ volatile("svc 0" : "+r"(r0)
		: "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7)
		: "memory", "cc");
	return r0;
#endif
}

static long open_file(const char *path, long flags)
{
	return syscall5(5, (long)path, flags, 0, 0, 0);
}

static void write_all(long fd, const char *data, long len)
{
	while (len > 0) {
		long ret = syscall5(4, fd, (long)data, len, 0, 0);

		if (ret == -4) /* EINTR */
			continue;
		if (ret <= 0)
			return;
		data += ret;
		len -= ret;
	}
}

static void log_message(long fd, const char *msg)
{
	long len = 0;

	while (msg[len])
		len++;
	write_all(fd, msg, len);
}

static void mount_fs(const char *type, const char *path)
{
	syscall5(21, (long)type, (long)path, (long)type, 0, 0);
}

static void log_file(long log, const char *path)
{
	char buf[512];
	long fd = open_file(path, 0);
	long len;

	log_message(log, "<6>WT6580: ");
	log_message(log, path);
	log_message(log, "\n");
	if (fd < 0) {
		log_message(log, "<3>WT6580: cannot open diagnostic file\n");
		return;
	}
	while ((len = syscall5(3, fd, (long)buf, sizeof(buf), 0, 0)) > 0)
		write_all(log, buf, len);
	syscall5(6, fd, 0, 0, 0, 0);
}

/* Keep USB backpressure/disconnects out of the watchdog feeding path. */
static long usb_fd = -1;
static long watchdog = -1;
static long dump_fd = -1;
static char usb_output[8192];
static long usb_output_len;
static long usb_output_pos;
static char usb_command[512];
static unsigned int usb_command_len;
static int usb_command_overflow;

static void usb_reply(const char *text)
{
	usb_output_len = 0;
	usb_output_pos = 0;
	while (*text && usb_output_len < (long)sizeof(usb_output))
		usb_output[usb_output_len++] = *text++;
}

static int text_equal(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void usb_start_dump(const char *path)
{
	dump_fd = open_file(path, O_NONBLOCK);
	if (dump_fd < 0)
		usb_reply("\n[WT6580 cannot open diagnostic file]\n");
}

#include "fbtest.h"
#include "drmtest.h"
#include "mmctest.h"
#include "shell.h"

static void sync_storage(void)
{
	static const char * const mounts[] = {
		"/mnt/data", "/mnt/cache", "/mnt/vendor", "/mnt/system",
	};
	unsigned int i;

	syscall5(36, 0, 0, 0, 0, 0);
	/* Best effort: stop ext4 journals cleanly before the watchdog restart. */
	for (i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++)
		syscall5(52, (long)mounts[i], 0, 0, 0, 0); /* umount2 */
}

static void usb_execute_command(void)
{
	if (usb_command_overflow) {
		usb_reply("\n[WT6580 command too long]\n");
	} else if (text_equal(usb_command, "help")) {
		usb_reply("\nWT6580 USB CDC debug: help, dmesg, status, cmdline, "
			  "interrupts, meminfo, fbstatus, fbtest, fbrestore, dsistate, panelid, "
			  "paneloff, panelon, panelreinit, drmstatus, drmtest, drmoff, mmcstatus, mmcid, partitions, mmcread, reboot, reboot bootloader\n"
			  "storage, mountall, umountall, mounts, fsrwtest, sh COMMAND\n"
			  "dmesg replays the kernel ring buffer, including pre-USB logs.\n");
	} else if (text_equal(usb_command, "storage")) {
		shell_start("/etc/wt6580-storage.sh scan");
	} else if (text_equal(usb_command, "mountall")) {
		shell_start("/etc/wt6580-storage.sh mount");
	} else if (text_equal(usb_command, "umountall")) {
		shell_start("/etc/wt6580-storage.sh umount");
	} else if (text_equal(usb_command, "fsrwtest")) {
		shell_start("/etc/wt6580-storage.sh test");
	} else if (text_equal(usb_command, "mounts")) {
		usb_start_dump("/proc/mounts");
	} else if (usb_command[0] == 's' && usb_command[1] == 'h' &&
		   usb_command[2] == ' ' && usb_command[3]) {
		shell_start(usb_command + 3);
	} else if (text_equal(usb_command, "mmcstatus")) {
		usb_start_dump("/sys/bus/platform/devices/11120000.mmc/state");
	} else if (text_equal(usb_command, "mmcid")) {
		usb_start_dump("/sys/block/mmcblk0/device/cid");
	} else if (text_equal(usb_command, "partitions")) {
		usb_start_dump("/proc/partitions");
	} else if (text_equal(usb_command, "mmcread")) {
		mmc_read_test();
	} else if (text_equal(usb_command, "drmstatus")) {
		drm_test(0);
	} else if (text_equal(usb_command, "drmtest")) {
		drm_test(1);
	} else if (text_equal(usb_command, "drmoff")) {
		drm_test(2);
	} else if (text_equal(usb_command, "dmesg")) {
		usb_start_dump("/dev/kmsg");
	} else if (text_equal(usb_command, "status")) {
		usb_start_dump("/proc/version");
	} else if (text_equal(usb_command, "cmdline")) {
		usb_start_dump("/proc/cmdline");
	} else if (text_equal(usb_command, "interrupts")) {
		usb_start_dump("/proc/interrupts");
	} else if (text_equal(usb_command, "meminfo")) {
		usb_start_dump("/proc/meminfo");
	} else if (text_equal(usb_command, "fbstatus")) {
		dump_fd = open_file("/sys/bus/platform/devices/14007000.display/state", O_NONBLOCK);
		if (dump_fd < 0)
			usb_start_dump("/sys/bus/platform/devices/14009000.rdma/state");
	} else if (text_equal(usb_command, "dsistate")) {
		dump_fd = open_file("/sys/bus/platform/devices/14007000.display/dsistate", O_NONBLOCK);
		if (dump_fd < 0)
			usb_start_dump("/sys/bus/platform/devices/14012000.dsi/state");
	} else if (text_equal(usb_command, "paneloff") ||
		   text_equal(usb_command, "panelon") ||
		   text_equal(usb_command, "panelreinit")) {
		const char *path = "/sys/bus/platform/devices/14007000.display/panel";
		const char *value = text_equal(usb_command, "paneloff") ? "off" :
			text_equal(usb_command, "panelon") ? "on" : "reinit";
		long len = 0, fd = open_file(path, 1);

		if (fd < 0) {
			usb_reply("\n[WT6580 panel control unavailable]\n");
			return;
		}
		while (value[len])
			len++;
		syscall5(4, fd, (long)value, len, 0, 0);
		syscall5(6, fd, 0, 0, 0, 0);
		usb_start_dump(path);
	} else if (text_equal(usb_command, "panelid")) {
		const char *path = "/sys/bus/platform/devices/14007000.display/panelid";
		long fd = open_file(path, 1);
		long ret;

		if (fd < 0) {
			usb_reply("\n[WT6580 DSI panel query unavailable]\n");
			return;
		}
		ret = syscall5(4, fd, (long)"1", 1, 0, 0);
		syscall5(6, fd, 0, 0, 0, 0);
		if (ret == 1)
			usb_start_dump(path);
		else
			usb_reply("\n[WT6580 DSI panel query failed]\n");
	} else if (text_equal(usb_command, "fbtest")) {
		fb_test(0);
	} else if (text_equal(usb_command, "fbrestore")) {
		fb_test(1);
	} else if (text_equal(usb_command, "reboot bootloader")) {
		long fd, ret;

		sync_storage(); /* Unmount before arming the RTC marker. */
		fd = open_file("/sys/bus/platform/devices/1000f000.pmic-wrap:reboot-mode/bootloader", 1);
		if (fd < 0) {
			usb_reply("\n[WT6580 bootloader mode driver unavailable; reboot cancelled]\n");
			return;
		}
		ret = syscall5(4, fd, (long)"1", 1, 0, 0);
		syscall5(6, fd, 0, 0, 0, 0);
		if (ret != 1) {
			usb_reply("\n[WT6580 RTC fastboot marker failed; reboot cancelled]\n");
			return;
		}
		log_message(usb_fd, "\n[WT6580 rebooting to bootloader]\n");
		/* RESTART2 passes the mode string through the kernel restart chain. */
		syscall5(88, 0xfee1deadUL, 672274793, 0xa1b2c3d4UL,
			 (long)"bootloader", 0);
		usb_reply("\n[WT6580 bootloader reboot failed]\n");
	} else if (text_equal(usb_command, "reboot")) {
		/* Best-effort notice: USB can disconnect before the host reads it. */
		log_message(usb_fd, "\n[WT6580 rebooting]\n");
		sync_storage();
		/* ARM EABI reboot(MAGIC1, MAGIC2, CMD_RESTART, NULL). */
		syscall5(88, 0xfee1deadUL, 672274793, 0x01234567, 0, 0);
		usb_reply("\n[WT6580 reboot failed]\n");
	} else {
		usb_reply("\n[WT6580 unknown command; type help]\n");
	}
}

static void usb_open_console(void)
{
	/* Linux ARM's TCGETS/TCSETS layout, kept independent of libc headers. */
	struct {
		unsigned int iflag, oflag, cflag, lflag;
		unsigned char line, cc[19];
	} termios;

	usb_fd = open_file("/dev/ttyGS0", 2 | O_NOCTTY | O_NONBLOCK);
	if (usb_fd < 0)
		return;
	if (syscall5(54, usb_fd, 0x5401, (long)&termios, 0, 0) == 0) {
		termios.iflag = 0x100; /* ICRNL: accept the terminal's Enter key. */
		termios.oflag = 0x5; /* OPOST | ONLCR: LF becomes CRLF on USB. */
		/* ICANON | ECHO | ECHOE | ECHOK: edit and echo commands locally. */
		termios.lflag = 0x3a;
		termios.cflag = (termios.cflag & ~0x170U) | 0x8b0U;
		termios.cc[2] = 127; /* VERASE */
		termios.cc[3] = 21; /* VKILL: Ctrl-U */
		termios.cc[6] = 0; /* VMIN */
		termios.cc[5] = 0; /* VTIME */
		syscall5(54, usb_fd, 0x5402, (long)&termios, 0, 0);
	}
	usb_command_len = 0;
	usb_command_overflow = 0;
}

static int usb_poll_console(void)
{
	long ret;
	unsigned int i;
	char ch;

	if (usb_fd < 0)
		usb_open_console();
	if (usb_fd < 0)
		return 0;

	if (usb_output_pos < usb_output_len) {
		ret = syscall5(4, usb_fd, (long)(usb_output + usb_output_pos),
			       usb_output_len - usb_output_pos, 0, 0);
		if (ret > 0)
			usb_output_pos += ret;
		else if (ret < 0 && ret != -11 && ret != -4) {
			syscall5(6, usb_fd, 0, 0, 0, 0);
			usb_fd = -1;
		}
		return ret > 0;
	}
	usb_output_len = 0;
	usb_output_pos = 0;
	if (dump_fd >= 0) {
		ret = syscall5(3, dump_fd, (long)usb_output, sizeof(usb_output), 0, 0);
		if (ret > 0) {
			usb_output_len = ret;
		} else if (shell_capture && (ret == -11 || ret == -4)) {
			return 0; /* An idle child pipe stays open; keep feeding the watchdog. */
		} else if (ret != -32 && ret != -4) {
			/* EPIPE on /dev/kmsg means records were overwritten: retry. */
			syscall5(6, dump_fd, 0, 0, 0, 0);
			dump_fd = -1;
			if (shell_capture)
				return 1;
			usb_reply(ret == 0 || ret == -11 ?
				  "\n[WT6580 dump complete]\n" :
				  "\n[WT6580 diagnostic read failed]\n");
		}
		return 1;
	}

	if (shell_capture) {
		shell_reap();
		if (shell_running)
			return 0;
		shell_result();
		return 1;
	}

	/* One completed command per poll; leave later commands in the TTY queue. */
	for (i = 0; i < sizeof(usb_command); i++) {
		ret = syscall5(3, usb_fd, (long)&ch, 1, 0, 0);
		if (ret <= 0) {
			/* With canonical O_NONBLOCK input, idle returns EAGAIN.
			 * EOF means hangup: discard this fd so a USB reconfiguration
			 * cannot leave PID 1 polling the dead TTY forever.
			 */
			if (ret == 0 || (ret != -11 && ret != -4)) {
				syscall5(6, usb_fd, 0, 0, 0, 0);
				usb_fd = -1;
			}
			return 0;
		}
		if (ch == '\r' || ch == '\n') {
			if (!usb_command_len && !usb_command_overflow)
				continue;
			usb_command[usb_command_len] = 0;
			usb_execute_command();
			usb_command_len = 0;
			usb_command_overflow = 0;
			return 1;
		}
		if ((ch == '\b' || ch == 127) && !usb_command_overflow) {
			if (usb_command_len)
				usb_command_len--;
		} else if (ch >= 32 && ch <= 126) {
			if (usb_command_len + 1 < sizeof(usb_command))
				usb_command[usb_command_len++] = ch;
			else
				usb_command_overflow = 1;
		}
	}
	return 1;
}

__attribute__((used, noreturn)) static void boot_main(void)
{
	long log;
	int timeout = 30;
	long next_feed = 0, next_heartbeat = 0;

	mount_fs("devtmpfs", "/dev");
	mount_fs("proc", "/proc");
	mount_fs("sysfs", "/sys");
	syscall5(39, (long)"/run", 0755, 0, 0, 0);
	mount_fs("tmpfs", "/run");
	syscall5(39, (long)"/tmp", 01777, 0, 0, 0);
	mount_fs("tmpfs", "/tmp");
	syscall5(39, (long)"/dev/pts", 0755, 0, 0, 0);
	mount_fs("devpts", "/dev/pts");
	syscall5(39, (long)"/sys/fs/pstore", 0755, 0, 0, 0);
	mount_fs("pstore", "/sys/fs/pstore");
	log = open_file("/dev/kmsg", 1);
	if (log < 0)
		log = open_file("/dev/console", 1);
	log_message(log, "<6>WT6580: DIAGNOSTIC_INIT_STARTED (ARMv7 PID 1)\n");

	watchdog = open_file("/dev/watchdog0", 1);
	if (watchdog < 0)
		watchdog = open_file("/dev/watchdog", 1);
	if (watchdog >= 0) {
		/* _IOWR('W', 6, int) == WDIOC_SETTIMEOUT on ARM EABI. */
		syscall5(54, watchdog, 0xc0045706UL, (long)&timeout, 0, 0);
		write_all(watchdog, "K", 1);
		log_message(log, "<6>WT6580: watchdog opened; keepalive active\n");
	} else {
		log_message(log, "<3>WT6580: watchdog device unavailable\n");
	}

	log_file(log, "/proc/version");
	log_file(log, "/proc/cmdline");
	log_file(log, "/proc/iomem");
	log_file(log, "/proc/interrupts");
	log_file(log, "/proc/meminfo");
	usb_reply("\nWT6580 diagnostic USB CDC ACM. Type help or dmesg.\n");

	for (;;) {
		struct { long sec, nsec; } now, delay = { 0, 1000000 };
		unsigned int budget = 512;

		/* ARM EABI clock_gettime(CLOCK_MONOTONIC), independent of USB load. */
		syscall5(263, 1, (long)&now, 0, 0, 0);

		if (watchdog >= 0 && now.sec >= next_feed) {
			write_all(watchdog, "K", 1);
			next_feed = now.sec + 5;
		}
		if (now.sec >= next_heartbeat) {
			next_heartbeat = now.sec + 60;
			log_message(log, "<6>WT6580: DIAGNOSTIC_HEARTBEAT\n");
			log_file(log, "/proc/uptime");
		}
		shell_reap();
		/* Drain ready records immediately, but bound work between watchdog checks. */
		while (budget-- && usb_poll_console())
			;
		if (dump_fd < 0 && usb_output_pos >= usb_output_len)
			delay.nsec = 20000000;
		syscall5(162, (long)&delay, 0, 0, 0, 0);
	}
}

#ifndef WT6580_INIT_TEST
__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile("mov fp, #0\nbl boot_main\nb .");
}
#endif
