// SPDX-License-Identifier: GPL-2.0-only
/* Test the actual diagnostic drawing code against bounded fake fbdev I/O. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

static long syscall5(long, long, long, long, long, long);
static long open_file(const char *, long);
static void usb_reply(const char *);
#define O_NOCTTY 0x100
#include "fbtest.h"

static unsigned char memory[480 * 800 * 4 + 32], original[sizeof(memory)];
static struct wt_fb_var var;
static struct wt_fb_fix fix;
static unsigned int pos, writes;
static const char *reply;

static long open_file(const char *path, long flags)
{
	assert(!strcmp(path, "/dev/fb0") && flags == (2 | O_NOCTTY));
	pos = 0;
	return 10;
}

static void usb_reply(const char *s) { reply = s; }

static long syscall5(long n, long fd, long a, long b, long d, long e)
{
	(void)d;
	(void)e;
	assert(fd == 10);
	switch (n) {
	case 54:
		if (a == 0x4600)
			memcpy((void *)b, &var, sizeof(var));
		else {
			assert(a == 0x4602);
			memcpy((void *)b, &fix, sizeof(fix));
		}
		return 0;
	case 19:
		assert(a >= 0 && a <= fix.len && b == 0);
		pos = a;
		return pos;
	case 3:
	case 4:
		if (b > 97)
			b = 97; /* Exercise partial reads/writes. */
		assert(pos + b <= fix.len);
		if (n == 3)
			memcpy((void *)a, memory + pos, b);
		else {
			memcpy(memory + pos, (void *)a, b);
			writes++;
		}
		pos += b;
		return b;
	case 6:
		return 0;
	default:
		assert(0);
		return -1;
	}
}

static void fixture(unsigned int bpp)
{
	memset(&var, 0, sizeof(var));
	memset(&fix, 0, sizeof(fix));
	memset(memory, 0x5a, sizeof(memory));
	memcpy(original, memory, sizeof(memory));
	var.xres = 480;
	var.yres = 800;
	var.bpp = bpp;
	var.red = (struct wt_fb_bitfield) { bpp == 16 ? 11 : 16, bpp == 16 ? 5 : 8, 0 };
	var.green = (struct wt_fb_bitfield) { bpp == 16 ? 5 : 8, bpp == 16 ? 6 : 8, 0 };
	var.blue = (struct wt_fb_bitfield) { 0, bpp == 16 ? 5 : 8, 0 };
	if (bpp == 32)
		var.alpha = (struct wt_fb_bitfield) { 24, 8, 0 };
	fix.start = 0xbfa00000;
	fix.visual = 2;
	fix.stride = 480 * (bpp / 8) + (bpp == 32 ? 0 : 16);
	fix.len = fix.stride * 800;
	fb_saved_size = writes = 0;
}

int main(void)
{
	static const unsigned int rgb565[] = {
		0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0,
	};
	static const unsigned int rgb888[] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0,
	};
	unsigned int bpp, y, x, i;

	for (bpp = 16; bpp <= 32; bpp += 8) {
		fixture(bpp);
		fb_test(1);
		assert(strstr(reply, "no matching") && !writes);
		fb_test(0);
		assert(strstr(reply, "color bars written"));
		for (y = 0; y < 800; y++) {
			unsigned char *line = memory + y * fix.stride;

			for (x = 0; x < 480; x++) {
				unsigned int expected = bpp == 16 ? rgb565[x / 60] : rgb888[x / 60];

				if (bpp == 32)
					expected |= 0xff000000;
				for (i = 0; i < bpp / 8; i++)
					assert(line[x * (bpp / 8) + i] == ((expected >> (8 * i)) & 255));
			}
			/* First white bar and final black bar, including opaque alpha. */
			for (x = 0; x < 60; x++)
				for (i = 0; i < bpp / 8; i++)
					assert(line[x * (bpp / 8) + i] == 255);
			for (x = 420; x < 480; x++)
				for (i = 0; i < bpp / 8; i++)
					assert(line[x * (bpp / 8) + i] == (bpp == 32 && i == 3 ? 255 : 0));
			for (x = 480 * (bpp / 8); x < fix.stride; x++)
				assert(line[x] == 0x5a); /* Do not touch pitch padding. */
		}
		assert(!memcmp(memory + fix.len, original + fix.len, sizeof(memory) - fix.len));
		fb_test(0); /* Repeated draws must retain the first snapshot. */
		fb_test(1);
		assert(strstr(reply, "restored"));
		assert(!memcmp(memory, original, sizeof(memory)));
	}
	fixture(32);
	var.xres = 481;
	fb_test(0);
	assert(strstr(reply, "unsupported") && !writes);
	fixture(32);
	var.red.offset = 32;
	fb_test(0);
	assert(strstr(reply, "unsupported") && !writes);
	puts("Framebuffer: 16/24/32-bit bars, partial I/O, padding, bounds, repeated draws and restoration passed");
	return 0;
}
