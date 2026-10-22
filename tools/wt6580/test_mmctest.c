// SPDX-License-Identifier: GPL-2.0-only
/* Verify ARM open flags, uncached passes, CRC reporting and error cleanup. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int opens, closes, reads, feeds, fault, interrupted;
static long watchdog = 7;
static char reply[256];

static void usb_reply(const char *s)
{
	assert(strlen(s) < sizeof(reply));
	strcpy(reply, s);
}

static long open_file(const char *path, long flags)
{
	assert(!strcmp(path, "/dev/mmcblk0"));
	/* ARM EABI: O_RDONLY | O_DIRECT | O_LARGEFILE. */
	assert(flags == 0x30000);
	opens++;
	return fault == 1 ? -2 : 9;
}

static void write_all(long fd, const char *buf, long size)
{
	assert(fd == watchdog && size == 1 && *buf == 'K');
	feeds++;
}

static long syscall5(long nr, long a, long b, long c, long d, long e)
{
	(void)d;
	(void)e;
	assert(a == 9);
	if (nr == 6) {
		closes++;
		return 0;
	}
	assert(nr == 3 && c == 65536 && !((unsigned long)b & 4095));
	if (fault == 4 && !interrupted++)
		return -4;
	reads++;
	memset((void *)b, fault == 2 && opens == 2 ? 1 : 0, c);
	return fault == 3 ? c - 512 : c;
}

#include "mmctest.h"

int main(void)
{
	assert((mmc_crc32(~0U, (const unsigned char *)"123456789", 9) ^ ~0U)
	       == 0xcbf43926);
	for (fault = 0; fault < 5; fault++) {
		opens = closes = reads = feeds = interrupted = 0;
		mmc_read_test();
		if (fault == 1) {
			assert(strstr(reply, "open failed") && opens == 1 && !closes && !reads);
		} else if (fault == 3) {
			assert(strstr(reply, "I/O failed") && opens == 1 && closes == 1 && reads == 1);
		} else {
			assert(opens == 2 && closes == 2 && reads == 32 && feeds == 32);
			if (fault == 2)
				assert(strstr(reply, "MISMATCH"));
			else
				assert(strstr(reply, "a738ea1c a738ea1c MATCH"));
		}
	}
	puts("eMMC read-only diagnostic tests passed");
	return 0;
}
