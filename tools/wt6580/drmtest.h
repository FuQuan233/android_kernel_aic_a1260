/* SPDX-License-Identifier: GPL-2.0-only */
/* ARM EABI DRM UAPI; diagnostic scanout uses GEM buffers, never /dev/mem. */
struct wt_drm_mode {
	unsigned int clock;
	unsigned short hdisplay, hsync_start, hsync_end, htotal, hskew;
	unsigned short vdisplay, vsync_start, vsync_end, vtotal, vscan;
	unsigned int vrefresh, flags, type;
	char name[32];
};
struct wt_drm_res {
	unsigned long long fbs, crtcs, connectors, encoders;
	unsigned int count_fbs, count_crtcs, count_connectors, count_encoders;
	unsigned int min_width, max_width, min_height, max_height;
};
struct wt_drm_conn {
	unsigned long long encoders, modes, props, values;
	unsigned int count_modes, count_props, count_encoders;
	unsigned int encoder, id, type, type_id, connection;
	unsigned int width, height, subpixel, pad;
};
struct wt_drm_crtc {
	unsigned long long connectors;
	unsigned int count_connectors, id, fb, x, y, gamma, valid;
	struct wt_drm_mode mode;
};
struct wt_drm_dumb {
	unsigned int height, width, bpp, flags, handle, pitch;
	unsigned long long size;
};
struct wt_drm_map { unsigned int handle, pad; unsigned long long offset; };
struct wt_drm_fb { unsigned int id, width, height, pitch, bpp, depth, handle; };
_Static_assert(sizeof(struct wt_drm_mode) == 68, "DRM mode ABI");
_Static_assert(sizeof(struct wt_drm_res) == 64, "DRM resources ABI");
_Static_assert(sizeof(struct wt_drm_conn) == 80, "DRM connector ABI");
_Static_assert(sizeof(struct wt_drm_crtc) == 104, "DRM CRTC ABI");
_Static_assert(sizeof(struct wt_drm_dumb) == 32, "DRM dumb ABI");
_Static_assert(sizeof(struct wt_drm_map) == 16, "DRM map ABI");
#define WT_DRM_IOWR(n, type) (0xc0006400U | ((unsigned int)sizeof(type) << 16) | (n))

static long drm_fd = -1;
static unsigned int drm_handle, drm_fb, drm_crtc_id, drm_pitch, drm_size;
static unsigned char *drm_pixels;

static long drm_ioctl(long fd, unsigned int op, void *arg)
{
	return syscall5(54, fd, op, (long)arg, 0, 0);
}

static long drm_mmap(long fd, unsigned int size, unsigned int page)
{
#ifdef WT6580_DRM_MMAP
	return WT6580_DRM_MMAP(fd, size, page);
#elif defined(WT6580_INIT_TEST)
	(void)fd;
	(void)size;
	(void)page;
	return -38;
#else
	register long r0 __asm__("r0") = 0;
	register long r1 __asm__("r1") = size;
	register long r2 __asm__("r2") = 3; /* PROT_READ | PROT_WRITE */
	register long r3 __asm__("r3") = 1; /* MAP_SHARED */
	register long r4 __asm__("r4") = fd;
	register long r5 __asm__("r5") = page;
	register long r7 __asm__("r7") = 192; /* mmap2, offset in 4 KiB pages */

	__asm__ volatile("svc 0" : "+r"(r0)
		: "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
		: "memory", "cc");
	return r0;
#endif
}

static void drm_result(const char *stage, long result)
{
	char text[160], digits[24];
	unsigned int n = 0, d = 0;
	unsigned long value;
	const char *prefix = "\n[WT6580 DRM ";

	while (*prefix)
		text[n++] = *prefix++;
	while (*stage && n < 100)
		text[n++] = *stage++;
	text[n++] = ':';
	text[n++] = ' ';
	if (result < 0) {
		text[n++] = '-';
		value = -(result + 1) + 1UL;
	} else {
		value = result;
	}
	do {
		digits[d++] = '0' + value % 10;
		value /= 10;
	} while (value);
	while (d)
		text[n++] = digits[--d];
	text[n++] = ']';
	text[n++] = '\n';
	text[n] = 0;
	usb_reply(text);
}

static void drm_draw(void)
{
	static const unsigned int colors[] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0x000000,
	};
	unsigned int x, y;

	for (y = 0; y < 800; y++) {
		unsigned int *row = (unsigned int *)(drm_pixels + y * drm_pitch);

		for (x = 0; x < 480; x++)
			row[x] = 0xff000000 | colors[x / 60];
	}
	/* Order write-combined framebuffer stores before the modeset ioctl. */
#ifndef WT6580_INIT_TEST
	__asm__ volatile("dsb sy" ::: "memory");
#endif
}

static void drm_cleanup(long fd)
{
	if (drm_pixels) {
		syscall5(91, (long)drm_pixels, drm_size, 0, 0, 0);
		drm_pixels = 0;
	}
	if (drm_fb)
		drm_ioctl(fd, WT_DRM_IOWR(0xaf, unsigned int), &drm_fb);
	if (drm_handle)
		drm_ioctl(fd, WT_DRM_IOWR(0xb4, unsigned int), &drm_handle);
	syscall5(6, fd, 0, 0, 0, 0);
	drm_fd = -1;
	drm_handle = drm_fb = drm_crtc_id = 0;
}

static void drm_test(int action)
{
	struct wt_drm_res res = { 0 };
	struct wt_drm_conn conn = { 0 };
	struct wt_drm_mode modes[8] = { 0 };
	struct wt_drm_crtc crtc = { 0 };
	struct wt_drm_dumb dumb = { .width = 480, .height = 800, .bpp = 32 };
	struct wt_drm_map map = { 0 };
	struct wt_drm_fb fb = { .width = 480, .height = 800, .bpp = 32, .depth = 24 };
	unsigned int crtcs[8], connectors[8], i;
	long fd, ret;
	const char *stage = "resources";

	if (action == 2) {
		if (drm_fd < 0) {
			usb_reply("\n[WT6580 DRM no diagnostic scanout active]\n");
			return;
		}
		crtc.id = drm_crtc_id;
		ret = drm_ioctl(drm_fd, WT_DRM_IOWR(0xa2, struct wt_drm_crtc), &crtc);
		if (!ret)
			drm_cleanup(drm_fd);
		drm_result("disable", ret);
		return;
	}
	if (action == 1 && drm_fd >= 0) {
		drm_draw();
		usb_reply("\n[WT6580 DRM color bars redrawn in active GEM buffer]\n");
		return;
	}
	fd = drm_fd >= 0 ? drm_fd : open_file("/dev/dri/card0", 2 | O_NOCTTY);
	if (fd < 0) {
		drm_result("open card0", fd);
		return;
	}
	ret = drm_ioctl(fd, WT_DRM_IOWR(0xa0, struct wt_drm_res), &res);
	if (ret)
		goto fail;
	if (!res.count_crtcs || res.count_crtcs > 8 ||
	    !res.count_connectors || res.count_connectors > 8) {
		ret = -19;
		goto fail;
	}
	/* Unrequested arrays must keep zero capacity on the second ioctl. */
	res.count_fbs = res.count_encoders = 0;
	res.crtcs = (unsigned long)crtcs;
	res.connectors = (unsigned long)connectors;
	ret = drm_ioctl(fd, WT_DRM_IOWR(0xa0, struct wt_drm_res), &res);
	if (ret)
		goto fail;
	if (res.count_crtcs > 8 || res.count_connectors > 8) {
		ret = -75;
		goto fail;
	}
	stage = "connected 480x800 mode";
	for (i = 0; i < res.count_connectors; i++) {
		conn.count_props = conn.count_encoders = 0;
		conn.id = connectors[i];
		conn.modes = (unsigned long)modes;
		/* Force the first probe: fbdev emulation is intentionally disabled. */
		conn.count_modes = 0;
		ret = drm_ioctl(fd, WT_DRM_IOWR(0xa7, struct wt_drm_conn), &conn);
		if (ret)
			continue;
		conn.count_props = conn.count_encoders = 0;
		conn.count_modes = 8;
		ret = drm_ioctl(fd, WT_DRM_IOWR(0xa7, struct wt_drm_conn), &conn);
		if (!ret && conn.connection == 1 && conn.count_modes &&
		    conn.count_modes <= 8 && modes[0].hdisplay == 480 &&
		    modes[0].vdisplay == 800)
			break;
	}
	if (i == res.count_connectors) {
		ret = -19;
		goto fail;
	}
	if (!action) {
		if (drm_fd < 0)
			syscall5(6, fd, 0, 0, 0, 0);
		usb_reply("\n[WT6580 DRM card0: connected DSI, 480x800 mode available]\n");
		return;
	}
	stage = "create dumb buffer";
	ret = drm_ioctl(fd, WT_DRM_IOWR(0xb2, struct wt_drm_dumb), &dumb);
	if (ret)
		goto fail;
	drm_handle = dumb.handle;
	if (dumb.pitch < 1920 || dumb.pitch > 8192 || dumb.pitch % 4 ||
	    dumb.size < (unsigned long long)dumb.pitch * 800 || dumb.size > 8 * 1024 * 1024) {
		ret = -22;
		goto fail;
	}
	drm_pitch = dumb.pitch;
	drm_size = dumb.size;
	map.handle = dumb.handle;
	stage = "map dumb buffer";
	ret = drm_ioctl(fd, WT_DRM_IOWR(0xb3, struct wt_drm_map), &map);
	if (ret)
		goto fail;
	if ((map.offset & 4095) || (map.offset >> 12) > 0xffffffffULL) {
		ret = -22;
		goto fail;
	}
	ret = drm_mmap(fd, drm_size, map.offset >> 12);
	if ((unsigned long)ret >= (unsigned long)-4095)
		goto fail;
	drm_pixels = (unsigned char *)ret;
	drm_draw();
	fb.pitch = dumb.pitch;
	fb.handle = dumb.handle;
	stage = "add framebuffer";
	ret = drm_ioctl(fd, WT_DRM_IOWR(0xae, struct wt_drm_fb), &fb);
	if (ret)
		goto fail;
	drm_fb = fb.id;
	crtc.id = crtcs[0];
	crtc.fb = fb.id;
	crtc.connectors = (unsigned long)&conn.id;
	crtc.count_connectors = 1;
	crtc.valid = 1;
	crtc.mode = modes[0];
	stage = "set CRTC";
	ret = drm_ioctl(fd, WT_DRM_IOWR(0xa2, struct wt_drm_crtc), &crtc);
	if (ret)
		goto fail;
	drm_crtc_id = crtc.id;
	drm_fd = fd;
	usb_reply("\n[WT6580 DRM modeset complete: GEM XRGB8888 480x800 color bars; drmoff disables]\n");
	return;
fail:
	if (drm_fd < 0)
		drm_cleanup(fd);
	drm_result(stage, ret);
}
