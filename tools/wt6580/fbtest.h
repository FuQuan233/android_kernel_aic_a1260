/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux ARM fbdev UAPI layouts; no libc or display-register access. */
struct wt_fb_bitfield { unsigned int offset, length, msb_right; };
struct wt_fb_var {
	unsigned int xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
	unsigned int bpp, grayscale;
	struct wt_fb_bitfield red, green, blue, alpha;
	unsigned int nonstd, activate, height, width, accel_flags;
	unsigned int pixclock, left, right, upper, lower, hsync, vsync;
	unsigned int sync, vmode, rotate, colorspace, reserved[4];
};
struct wt_fb_fix {
	char id[16];
	unsigned int start, len, type, type_aux, visual;
	unsigned short xpanstep, ypanstep, ywrapstep;
	unsigned int stride, mmio_start, mmio_len, accel;
	unsigned short capabilities, reserved[2];
};
_Static_assert(sizeof(struct wt_fb_var) == 160, "ARM fb_var_screeninfo ABI");
_Static_assert(sizeof(struct wt_fb_fix) == 68, "ARM fb_fix_screeninfo ABI");

static unsigned char fb_saved[480 * 800 * 4];
static unsigned int fb_saved_size, fb_saved_start;

static int fb_read_all(long fd, unsigned char *buf, unsigned int size)
{
	while (size) {
		long n = syscall5(3, fd, (long)buf, size, 0, 0);

		if (n == -4)
			continue;
		if (n <= 0)
			return -1;
		buf += n;
		size -= n;
	}
	return 0;
}

static int fb_write_all(long fd, const unsigned char *buf, unsigned int size)
{
	while (size) {
		long n = syscall5(4, fd, (long)buf, size, 0, 0);

		if (n == -4)
			continue;
		if (n <= 0)
			return -1;
		buf += n;
		size -= n;
	}
	return 0;
}

static unsigned int fb_component(unsigned int value, struct wt_fb_bitfield f)
{
	if (!f.length)
		return 0;
	return ((value * ((1U << f.length) - 1) + 127) / 255) << f.offset;
}

static int fb_field_valid(struct wt_fb_bitfield f, unsigned int bpp)
{
	return f.length <= 8 && f.offset <= bpp && f.length <= bpp - f.offset &&
		!f.msb_right;
}

static void fb_test(int restore)
{
	static const unsigned int colors[] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0x000000,
	};
	struct wt_fb_var v;
	struct wt_fb_fix f;
	unsigned char row[480 * 4];
	unsigned int size, x, y, byte, bytes, color, pixel;
	long fd = open_file("/dev/fb0", 2 | O_NOCTTY);
	const char *reply = "\n[WT6580 framebuffer unavailable; run fbstatus]\n";

	if (fd < 0)
		goto done;
	if (syscall5(54, fd, 0x4600, (long)&v, 0, 0) ||
	    syscall5(54, fd, 0x4602, (long)&f, 0, 0))
		goto close;
	if (v.xres != 480 || v.yres != 800 || v.xoffset || v.yoffset ||
	    v.grayscale || v.nonstd || f.type || f.visual != 2 ||
	    (v.bpp != 16 && v.bpp != 24 && v.bpp != 32) ||
	    f.stride > sizeof(fb_saved) / 800 || f.stride < 480 * (v.bpp / 8) ||
	    f.len < f.stride * 800 || !fb_field_valid(v.red, v.bpp) ||
	    !fb_field_valid(v.green, v.bpp) || !fb_field_valid(v.blue, v.bpp) ||
	    !fb_field_valid(v.alpha, v.bpp) || !v.red.length || !v.green.length ||
	    !v.blue.length) {
		reply = "\n[WT6580 unsupported framebuffer geometry/format]\n";
		goto close;
	}
	size = f.stride * 800;
	if (restore) {
		if (fb_saved_size != size || fb_saved_start != f.start) {
			reply = "\n[WT6580 no matching framebuffer snapshot]\n";
			goto close;
		}
		if (fb_write_all(fd, fb_saved, size))
			goto failed;
		fb_saved_size = 0;
		reply = "\n[WT6580 framebuffer restored]\n";
		goto close;
	}
	if (!fb_saved_size) {
		if (fb_read_all(fd, fb_saved, size))
			goto failed;
		fb_saved_start = f.start;
		fb_saved_size = size;
	} else if (fb_saved_size != size || fb_saved_start != f.start) {
		reply = "\n[WT6580 framebuffer changed; snapshot retained]\n";
		goto close;
	}
	if (syscall5(19, fd, 0, 0, 0, 0) < 0)
		goto failed;
	bytes = v.bpp / 8;
	for (x = 0; x < 480; x++) {
		color = colors[x / 60];
		pixel = fb_component(color >> 16, v.red) |
			fb_component((color >> 8) & 255, v.green) |
			fb_component(color & 255, v.blue) |
			fb_component(255, v.alpha);
		for (byte = 0; byte < bytes; byte++)
			row[x * bytes + byte] = pixel >> (byte * 8);
	}
	for (y = 0; y < 800; y++) {
		if (syscall5(19, fd, y * f.stride, 0, 0, 0) < 0 ||
		    fb_write_all(fd, row, 480 * bytes))
			goto failed;
	}
	reply = "\n[WT6580 color bars written: white yellow cyan green magenta red blue black; fbrestore restores LK image]\n";
	goto close;
failed:
	reply = "\n[WT6580 framebuffer I/O failed; snapshot retained if available]\n";
close:
	syscall5(6, fd, 0, 0, 0, 0);
done:
	usb_reply(reply);
}
