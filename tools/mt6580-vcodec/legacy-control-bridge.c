// SPDX-License-Identifier: GPL-2.0-only
/* Isolated ARM32 legacy control -> native, owned decoder jobs. */
#define _LARGEFILE64_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../../include/uapi/linux/mt6580-vcodec.h"
#include "val-buffer-bridge.h"

#define SLOTS 32
#define LEGACY(n) (0x40044d00U | (n))
#define VDEC_BASE 0x1500b000ULL
#define VDEC_SIZE 0x2000
#define AUX_BASE 0x15009000ULL
#define AUX_SIZE 0x1000
struct legacy_lock { uint32_t handle, handle_size, lock, timeout,
	reserved, reserved_size, type; uint8_t secure, pad[3]; };
struct legacy_isr { uint32_t handle, handle_size, type, isr_fn,
	reserved, reserved_size, timeout, count, status[16]; };
struct legacy_tids { uint32_t first, second, count, ids[16]; };
struct oal_header { uint32_t completed, timeout, count, status; };
struct oal_status { uint32_t address, value; };
struct context {
	struct legacy_tids tids;
	uintptr_t header;
	uint32_t handle, driver_type;
};
struct regmap { _Atomic uintptr_t va; _Atomic size_t size; _Atomic uint32_t offset; };
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static int legacy_fds[128];
static struct context contexts[SLOTS];
static struct regmap registers[SLOTS];
static unsigned int dec_users, enc_users;
static pid_t control_pid;
static void *(*next_dlopen)(const char *, int);
static pthread_once_t loader_once = PTHREAD_ONCE_INIT;
_Static_assert(sizeof(struct legacy_lock) == 32, "lock32");
_Static_assert(sizeof(struct legacy_isr) == 96, "isr32");
_Static_assert(sizeof(struct legacy_tids) == 76, "tids32");

__attribute__((constructor)) static void control_init(void)
{
	control_pid = getpid();
}
static int driver_type_valid(uint32_t type)
{
	return type == MT6580_VCODEC_H264_DRIVER ||
		type == MT6580_VCODEC_MP4_ENC_DRIVER;
}
static int raw_ioctl(int fd, unsigned long cmd, void *arg)
{
	return syscall(__NR_ioctl, fd, cmd, arg);
}
static int is_legacy(int fd)
{
	if (fd < 0 || fd == INT32_MAX) return 0;
	for (unsigned int i = 0; i < 128; i++)
		if (__atomic_load_n(&legacy_fds[i], __ATOMIC_ACQUIRE) == fd + 1) return 1;
	return 0;
}
static int open_impl(const char *path, int flags, mode_t mode)
{
	if (!strcmp(path, "/dev/Vcodec")) {
		int fd, base;
		if (control_pid != getpid()) { errno = EPERM; return -1; }
		base = mt6580_bridge_fd();
		if (base < 0) return -1;
		fd = fcntl(base, F_DUPFD_CLOEXEC, 3);
		if (fd < 0) return -1;
		pthread_mutex_lock(&control);
		for (unsigned int i = 0; i < 128; i++) if (!legacy_fds[i]) {
			__atomic_store_n(&legacy_fds[i], fd + 1, __ATOMIC_RELEASE);
			pthread_mutex_unlock(&control);
			fprintf(stderr, "LEGACY_OPEN fd=%d native=%d\n", fd, base);
			return fd;
		}
		pthread_mutex_unlock(&control);
		syscall(__NR_close, fd);
		errno = EMFILE;
		return -1;
	}
	return syscall(__NR_openat, AT_FDCWD, path, flags, mode);
}
int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
	}
	return open_impl(path, flags, mode);
}
int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
	}
	return open_impl(path, flags, mode);
}
int __open_2(const char *path, int flags)
{
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) { errno = EINVAL; return -1; }
	return open_impl(path, flags, 0);
}
int __open64_2(const char *path, int flags) { return __open_2(path, flags); }
int close(int fd)
{
	if (is_legacy(fd)) {
		pthread_mutex_lock(&control);
		for (unsigned int i = 0; i < 128; i++)
			if (legacy_fds[i] == fd + 1)
				__atomic_store_n(&legacy_fds[i], 0, __ATOMIC_RELEASE);
		pthread_mutex_unlock(&control);
	}
	return syscall(__NR_close, fd);
}
static struct context *thread_context(void)
{
	uint32_t tid = syscall(__NR_gettid);
	for (unsigned int i = 0; i < SLOTS; i++)
		for (unsigned int j = 0; j < contexts[i].tids.count; j++)
			if (contexts[i].tids.ids[j] == tid) return &contexts[i];
	return NULL;
}
/* IRQ completion drops the hardware owner, not its per-file result record.
 * DeInit must release that record before forgetting the native cookie. A live
 * job is cancelled by the kernel's normal IRQ synchronization/ISP-OFF path;
 * an unsafe stop retains its quarantine references independently of this ctx.
 */
static int release_context_job(struct context *ctx, int fd)
{
	struct mt6580_vcodec_job job = {
		.driver_type = ctx->driver_type,
		.handle = ctx->handle,
	};
	int rc;

	if (!ctx->handle) return 0;
	if (!driver_type_valid(ctx->driver_type)) { errno = EINVAL; return -1; }
	rc = raw_ioctl(fd, MT6580_VCODEC_JOB_RELEASE, &job);
	/* EALREADY removed a completed record; ENOENT proves it is gone. */
	if (rc && errno != EALREADY && errno != ENOENT) return rc;
	ctx->handle = 0;
	ctx->driver_type = 0;
	return 0;
}
static int bind_oal(struct context *ctx, uintptr_t address)
{
	struct mt6580_vcodec_oal bind = {0};
	struct oal_header *header = (void *)address;
	struct oal_status *status;
	if (mt6580_bridge_buffer(address, sizeof(*header), &bind.header_id,
				&bind.header_offset, 1)) return -1;
	if (header->count > 16) { errno = EINVAL; return -1; }
	bind.count = header->count;
	status = (void *)(uintptr_t)header->status;
	if (bind.count && mt6580_bridge_buffer((uintptr_t)status,
		bind.count * sizeof(*status), &bind.status_id, &bind.status_offset, 1)) return -1;
	for (unsigned int i = 0; i < bind.count; i++) {
		unsigned int j;
		if (status[i].address & 3) { errno = EINVAL; return -1; }
		for (j = 0; j < SLOTS; j++) {
			uintptr_t va = status[i].address;
			if (!registers[j].size || va < registers[j].va ||
			    va - registers[j].va > registers[j].size - 4) continue;
			bind.register_offsets[i] = registers[j].offset + va - registers[j].va;
			break;
		}
		if (j == SLOTS) { errno = EFAULT; return -1; }
	}
	int rc = raw_ioctl(mt6580_bridge_fd(), MT6580_VCODEC_OAL_BIND, &bind);
	if (!rc) ctx->header = address;
	fprintf(stderr, "OAL_BIND head=%08lx registers=%u rc=%d errno=%d\n",
		(unsigned long)address, bind.count, rc, errno);
	return rc;
}

int ioctl(int fd, int command, ...)
{
	unsigned long cmd = (uint32_t)command;
	va_list ap; void *arg;
	va_start(ap, command); arg = va_arg(ap, void *); va_end(ap);
	int rc = -1, saved;
	if ((cmd & 0xff00) != 0x4d00) return raw_ioctl(fd, cmd, arg);
	pthread_mutex_lock(&control);
	int legacy = is_legacy(fd);
	pthread_mutex_unlock(&control);
	if (!legacy) return raw_ioctl(fd, cmd, arg);
	if (control_pid != getpid()) { errno = EPERM; return -1; }
	pthread_mutex_lock(&control);
	struct context *ctx = thread_context();
	switch (cmd) {
	case LEGACY(0x24): {
		struct legacy_tids *t = arg;
		if (!t || !t->count || t->count > 16) { errno = EINVAL; break; }
		fprintf(stderr, "THREAD_IDS count=%u ids=", t->count);
		for (unsigned int i = 0; i < t->count; i++) fprintf(stderr, "%u,", t->ids[i]);
		fputc('\n', stderr);
		if (!ctx) for (unsigned int i = 0; i < SLOTS; i++)
			if (!contexts[i].tids.count) { ctx = &contexts[i]; break; }
		if (!ctx) { errno = ENOSPC; break; }
		for (unsigned int i = 0; i < t->count; i++) {
			char path[64]; snprintf(path, sizeof(path), "/proc/self/task/%u", t->ids[i]);
			if (access(path, F_OK)) { ctx = NULL; break; }
		}
		if (!ctx) { errno = EINVAL; break; }
		ctx->tids = (struct legacy_tids){ .count = t->count };
		memcpy(ctx->tids.ids, t->ids, t->count * sizeof(t->ids[0]));
		rc = 0;
		break;
	}
	case LEGACY(0x20):
		if (!ctx) { errno = EINVAL; break; }
		rc = bind_oal(ctx, (uintptr_t)arg);
		break;
	case LEGACY(0x21):
		ctx = NULL;
		for (unsigned int i = 0; i < SLOTS; i++)
			if (contexts[i].header == (uintptr_t)arg && arg) { ctx = &contexts[i]; break; }
		if (!ctx) { errno = EINVAL; break; }
		if (release_context_job(ctx, fd)) break;
		rc = raw_ioctl(fd, MT6580_VCODEC_OAL_UNBIND, NULL);
		if (!rc) memset(ctx, 0, sizeof(*ctx));
		break;
	case LEGACY(0x0d): {
		struct legacy_lock *old = arg;
		struct mt6580_vcodec_job job = {0};
		if (!ctx || !ctx->header || !old || !driver_type_valid(old->type) || !old->handle) {
			errno = EINVAL; break;
		}
		if (ctx->handle && (ctx->handle != old->handle || ctx->driver_type != old->type)) {
			struct mt6580_vcodec_job previous = {
				.driver_type = ctx->driver_type,
				.handle = ctx->handle,
			};
			/* Do not overwrite a cookie or cancel an in-flight older job just
			 * because this context asks for a different lock. */
			if (raw_ioctl(fd, MT6580_VCODEC_JOB_WAIT, &previous)) {
				if (errno == EAGAIN) errno = EBUSY;
				break;
			}
			if (release_context_job(ctx, fd)) break;
		}
		if (bind_oal(ctx, ctx->header)) break;
		if (mt6580_bridge_sync(0)) break;
		job.driver_type = old->type; job.timeout_ms = old->timeout; job.handle = old->handle;
		rc = raw_ioctl(fd, MT6580_VCODEC_JOB_ACQUIRE, &job);
		if (!rc) {
			ctx->handle = old->handle;
			ctx->driver_type = old->type;
		}
		break;
	}
	case LEGACY(0x0b): {
		struct legacy_isr *old = arg;
		struct mt6580_vcodec_job job = {0};
		if (!ctx || !old || !driver_type_valid(old->type)) { errno = EINVAL; break; }
		job.driver_type = old->type; job.timeout_ms = old->timeout; job.handle = old->handle;
		/* WAIT may sleep; release the userspace bookkeeping lock for workers. */
		pthread_mutex_unlock(&control);
		rc = raw_ioctl(fd, MT6580_VCODEC_JOB_WAIT, &job);
		saved = errno;
		if (!rc && mt6580_bridge_sync(1)) { rc = -1; saved = errno; }
		pthread_mutex_lock(&control);
		if (rc && saved == ETIMEDOUT) saved = ENOENT; /* BSP WAIT timeout ABI */
		errno = saved;
		break;
	}
	case LEGACY(0x33): {
		struct legacy_lock *old = arg;
		struct mt6580_vcodec_job job = {0};
		if (!ctx || !old || !driver_type_valid(old->type)) { errno = EINVAL; break; }
		job.driver_type = old->type; job.handle = old->handle;
		rc = raw_ioctl(fd, MT6580_VCODEC_JOB_RELEASE, &job);
		if (rc < 0 && errno == EALREADY) rc = 0;
		if (!rc) {
			if (ctx->handle == old->handle && ctx->driver_type == old->type) {
				ctx->handle = 0;
				ctx->driver_type = 0;
			}
			mt6580_bridge_sync(1);
		}
		break;
	}
	case LEGACY(0x15):
		if (!arg) { errno = EFAULT; break; }
		*(uint32_t *)arg = ++enc_users; rc = 0; break;
	case LEGACY(0x16):
		if (!arg || !enc_users) { errno = EINVAL; break; }
		*(uint32_t *)arg = --enc_users; rc = 0; break;
	case LEGACY(0x17):
		if (!arg) { errno = EFAULT; break; }
		*(uint32_t *)arg = ++dec_users; rc = 0; break;
	case LEGACY(0x18):
		if (!arg || !dec_users) { errno = EINVAL; break; }
		*(uint32_t *)arg = --dec_users; rc = 0; break;
	case LEGACY(0x27): case LEGACY(0x28):
		/* BSP's codec power-user commands do not program hardware;
		 * actual power is acquired/released with LOCK/IRQ/WAIT above.
		 */
		if (!arg || !driver_type_valid(((uint32_t *)arg)[2])) { errno = EINVAL; break; }
		rc = 0; break;
	case LEGACY(0x34):
		__sync_synchronize(); rc = 0; break;
	case LEGACY(0x31):
		if (!arg) { errno = EFAULT; break; }
		*(uint32_t *)arg = sysconf(_SC_NPROCESSORS_ONLN); rc = 0; break;
	default:
		errno = ENOTTY;
	}
	saved = errno;
	fprintf(stderr, "LEGACY_IOCTL cmd=%08lx tid=%ld rc=%d errno=%d\n",
		cmd, syscall(__NR_gettid), rc, saved);
	pthread_mutex_unlock(&control);
	errno = saved;
	return rc < 0 ? rc : 255;
}

void *mmap64(void *address, size_t length, int prot, int flags, int fd, off64_t offset)
{
	int legacy;
	void *result;
	legacy = is_legacy(fd);
	if (legacy) {
		fprintf(stderr, "LEGACY_MMAP fd=%d physical=%llx bytes=%zu\n", fd, (unsigned long long)offset, length);
		if ((uint64_t)offset >= VDEC_BASE && (uint64_t)offset < VDEC_BASE + VDEC_SIZE &&
		    length && length <= VDEC_BASE + VDEC_SIZE - offset) {
			offset = MT6580_VCODEC_MMAP_REGS + offset - VDEC_BASE;
		} else if ((uint64_t)offset >= AUX_BASE && (uint64_t)offset < AUX_BASE + AUX_SIZE &&
			   length && length <= AUX_BASE + AUX_SIZE - offset) {
			offset = MT6580_VCODEC_MMAP_REGS + VDEC_SIZE + offset - AUX_BASE;
		} else {
			errno = EINVAL; return MAP_FAILED;
		}
	}
	if (offset < 0 || (offset & 4095) || ((uint64_t)offset >> 12) > UINT32_MAX) {
		errno = EINVAL; return MAP_FAILED;
	}
	result = (void *)syscall(__NR_mmap2, address, length, prot, flags, fd,
				(unsigned long)((uint64_t)offset >> 12));
	if (legacy && result == MAP_FAILED) fprintf(stderr, "VDEC_MMAP_FAIL prot=%x flags=%x offset=%llx errno=%d\n", prot, flags, (unsigned long long)offset, errno);
	if (!legacy || result == MAP_FAILED) return result;
	pthread_mutex_lock(&control);
	for (unsigned int i = 0; i < SLOTS; i++) if (!registers[i].size) {
		registers[i].offset = (uint32_t)(offset - MT6580_VCODEC_MMAP_REGS);
		registers[i].va = (uintptr_t)result;
		registers[i].size = length;
		pthread_mutex_unlock(&control);
		fprintf(stderr, "VDEC_MMAP va=%p bytes=%u offset=%x\n", result,
			(unsigned int)length, (unsigned int)(offset - MT6580_VCODEC_MMAP_REGS));
		return result;
	}
	pthread_mutex_unlock(&control);
	syscall(__NR_munmap, result, length);
	errno = ENOSPC;
	return MAP_FAILED;
}
void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
	return mmap64(address, length, prot, flags, fd, offset);
}
int munmap(void *address, size_t length)
{
	int rc = syscall(__NR_munmap, address, length);
	if (!rc) {
		int found = 0;
		for (unsigned int i = 0; i < SLOTS; i++) {
			uintptr_t va = registers[i].va;
			size_t size = registers[i].size;
			if (size && (uintptr_t)address < va + size &&
			    va < (uintptr_t)address + length) found = 1;
		}
		if (found) {
			pthread_mutex_lock(&control);
			for (unsigned int i = 0; i < SLOTS; i++)
				if (registers[i].size && (uintptr_t)address < registers[i].va + registers[i].size &&
				    registers[i].va < (uintptr_t)address + length)
				{
					registers[i].size = 0;
					registers[i].va = 0;
					registers[i].offset = 0;
				}
			pthread_mutex_unlock(&control);
		}
	}
	return rc;
}
static void loader_init(void) { next_dlopen = dlsym(RTLD_NEXT, "dlopen"); }
void *dlopen(const char *path, int flags)
{
	pthread_once(&loader_once, loader_init);
	const char *directory = getenv("MT6580_CODEC_LIBDIR");
	char mapped[512];
	if (path && directory && !strncmp(path, "/vendor/lib/", 12)) {
		const char *name = path + 12;
		if (!strcmp(name, "libh264dec_xa.ca7.so") || !strcmp(name, "libh264dec_xb.ca7.so") ||
		    !strcmp(name, "libh264dec_sd.ca7.so") || !strcmp(name, "libmp4enc_xa.ca7.so") ||
		    !strcmp(name, "libpowerhalwrap.so")) {
			int n = snprintf(mapped, sizeof(mapped), "%s/%s", directory, name);
			if (n < 0 || n >= (int)sizeof(mapped)) { errno = ENAMETOOLONG; return NULL; }
			fprintf(stderr, "LEGACY_DLOPEN %s\n", mapped);
			path = mapped;
		}
	}
	return next_dlopen ? next_dlopen(path, flags) : NULL;
}
