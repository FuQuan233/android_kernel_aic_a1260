// SPDX-License-Identifier: GPL-2.0-only
/* Validate the diagnostic DRM client against the kernel's actual UAPI. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static long mock_syscall(long nr, long a, long b, long c, long d, long e);
static long mock_mmap(long fd, unsigned int size, unsigned int page);
#define WT6580_INIT_TEST mock_syscall
#define WT6580_DRM_MMAP mock_mmap
#include "init.c"

_Static_assert(sizeof(struct wt_drm_conn) == sizeof(struct drm_mode_get_connector), "connector");
_Static_assert(sizeof(struct wt_drm_crtc) == sizeof(struct drm_mode_crtc), "crtc");
_Static_assert(WT_DRM_IOWR(0xa0, struct wt_drm_res) == DRM_IOCTL_MODE_GETRESOURCES, "resources ioctl");
_Static_assert(WT_DRM_IOWR(0xa7, struct wt_drm_conn) == DRM_IOCTL_MODE_GETCONNECTOR, "connector ioctl");
_Static_assert(WT_DRM_IOWR(0xa2, struct wt_drm_crtc) == DRM_IOCTL_MODE_SETCRTC, "crtc ioctl");
_Static_assert(WT_DRM_IOWR(0xb2, struct wt_drm_dumb) == DRM_IOCTL_MODE_CREATE_DUMB, "dumb ioctl");
_Static_assert(WT_DRM_IOWR(0xae, struct wt_drm_fb) == DRM_IOCTL_MODE_ADDFB, "fb ioctl");
_Static_assert(offsetof(struct wt_drm_crtc, mode) == offsetof(struct drm_mode_crtc, mode), "mode");

static unsigned int pixels[484 * 800];
static int creates, modesets, disables, closes, removes, destroys, unmaps;
static unsigned int fail_op;
static int huge_pitch, disconnected, probed;

static long mock_mmap(long fd, unsigned int size, unsigned int page)
{
	assert(fd == 30 && size == sizeof(pixels) && page == 0x100001);
	return (long)pixels;
}

static long mock_syscall(long nr, long a, long b, long c, long d, long e)
{
	(void)d;
	(void)e;
	if (nr == 5) {
		assert(!strcmp((const char *)a, "/dev/dri/card0"));
		probed = 0;
		return 30;
	}
	if (nr == 6) {
		assert(a == 30);
		closes++;
		return 0;
	}
	if (nr == 91) {
		assert((void *)a == pixels && b == sizeof(pixels));
		unmaps++;
		return 0;
	}
	assert(nr == 54 && a == 30);
	if ((unsigned int)b == fail_op)
		return -5;
	switch ((unsigned int)b) {
	case DRM_IOCTL_MODE_GETRESOURCES: {
		struct drm_mode_card_res *r = (void *)c;

		if (r->count_encoders && !r->encoder_id_ptr)
			return -14;
		r->count_encoders = 1;
		r->count_crtcs = r->count_connectors = 1;
		if (r->crtc_id_ptr)
			*(unsigned int *)(unsigned long)r->crtc_id_ptr = 7;
		if (r->connector_id_ptr)
			*(unsigned int *)(unsigned long)r->connector_id_ptr = 9;
		return 0;
	}
	case DRM_IOCTL_MODE_GETCONNECTOR: {
		struct drm_mode_get_connector *r = (void *)c;
		struct drm_mode_modeinfo *m = (void *)(unsigned long)r->modes_ptr;

		assert(r->connector_id == 9 && (r->count_modes == 8 || !r->count_modes));
		if (!r->count_modes)
			probed = 1;
		assert(probed);
		r->connection = disconnected ? 2 : 1;
		r->count_modes = 1;
		m->hdisplay = 480;
		m->vdisplay = 800;
		return 0;
	}
	case DRM_IOCTL_MODE_CREATE_DUMB: {
		struct drm_mode_create_dumb *r = (void *)c;

		assert(r->width == 480 && r->height == 800 && r->bpp == 32 && !r->flags);
		r->handle = 55;
		r->pitch = huge_pitch ? 10000 : 1936;
		r->size = sizeof(pixels);
		creates++;
		return 0;
	}
	case DRM_IOCTL_MODE_MAP_DUMB: {
		struct drm_mode_map_dumb *r = (void *)c;

		assert(r->handle == 55);
		r->offset = 0x100001000ULL;
		return 0;
	}
	case DRM_IOCTL_MODE_ADDFB: {
		struct drm_mode_fb_cmd *r = (void *)c;

		assert(r->handle == 55 && r->pitch == 1936 && r->bpp == 32 && r->depth == 24);
		r->fb_id = 42;
		return 0;
	}
	case DRM_IOCTL_MODE_SETCRTC: {
		struct drm_mode_crtc *r = (void *)c;

		assert(r->crtc_id == 7);
		if (r->mode_valid) {
			assert(r->fb_id == 42 && r->count_connectors == 1);
			assert(*(unsigned int *)(unsigned long)r->set_connectors_ptr == 9);
			assert(r->mode.hdisplay == 480 && r->mode.vdisplay == 800);
			modesets++;
		} else {
			assert(!r->fb_id && !r->count_connectors);
			disables++;
		}
		return 0;
	}
	case DRM_IOCTL_MODE_RMFB:
		assert(*(unsigned int *)c == 42);
		removes++;
		return 0;
	case DRM_IOCTL_MODE_DESTROY_DUMB:
		assert(*(unsigned int *)c == 55);
		destroys++;
		return 0;
	default:
		assert(0);
	}
	return -38;
}

int main(void)
{
	static const unsigned int colors[] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};

	memset(pixels, 0x5a, sizeof(pixels));
	drm_test(0);
	assert(closes == 1 && !creates && drm_fd < 0);
	drm_test(1);
	assert(drm_fd == 30 && modesets == 1 && creates == 1);
	for (unsigned int y = 0; y < 800; y++) {
		for (unsigned int x = 0; x < 480; x++)
			assert(pixels[y * 484 + x] == colors[x / 60]);
		for (unsigned int x = 480; x < 484; x++)
			assert(pixels[y * 484 + x] == 0x5a5a5a5a);
	}
	drm_test(1);
	assert(creates == 1 && modesets == 1);
	drm_test(2);
	assert(drm_fd < 0 && disables == 1 && destroys == 1 && removes == 1 && unmaps == 1);
	fail_op = DRM_IOCTL_MODE_SETCRTC;
	drm_test(1);
	assert(drm_fd < 0 && destroys == 2 && removes == 2 && unmaps == 2);
	fail_op = 0;
	huge_pitch = 1;
	drm_test(1);
	assert(drm_fd < 0 && destroys == 3 && removes == 2 && unmaps == 2);
	huge_pitch = 0;
	disconnected = 1;
	drm_test(1);
	assert(drm_fd < 0 && creates == 3);
	puts("DRM client: actual UAPI/ABI, modeset, 4 GiB mmap offset, every pixel, padding, reuse, disable and failure cleanup passed");
}
