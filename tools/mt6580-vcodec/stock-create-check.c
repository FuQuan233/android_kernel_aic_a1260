// SPDX-License-Identifier: GPL-2.0-only
/* Run with the private VAL bridge preloaded and isolated stock dependencies. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
struct val_memory {
	uint32_t signature, type;
	uint64_t size, va, iova;
	uint32_t alignment, pad;
	uint64_t aligned_va, aligned_iova;
	uint32_t codec, share_fd;
	uint64_t ion_handle, reserved, reserved_size;
	uint32_t ion_fd, pad_end;
};
_Static_assert(sizeof(struct val_memory) == 96, "VAL ABI");
int main(int argc, char **argv)
{
	int (*create)(void **, uint32_t), (*release)(void *);
	int (*allocate)(struct val_memory *, uint32_t);
	int (*free_mem)(struct val_memory *, uint32_t);
	int (*flush)(struct val_memory *, uint32_t, uint32_t);
	int (*invalidate)(struct val_memory *, uint32_t, uint32_t);
	uint32_t (*map)(void *, uint32_t, uint32_t *, uint32_t, void *);
	uint32_t (*init_mva)(void **), (*deinit_mva)(void *);
	void *lib, *instance, *mva;
	setbuf(stdout, NULL);
	if (argc != 2) return 2;
	lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!lib) { puts(dlerror()); return 3; }
	create = dlsym(lib, "eVDecDrvCreate");
	release = dlsym(lib, "eVDecDrvRelease");
	allocate = dlsym(RTLD_DEFAULT, "eVideoMemAlloc");
	free_mem = dlsym(RTLD_DEFAULT, "eVideoMemFree");
	flush = dlsym(RTLD_DEFAULT, "eVideoFlushCache");
	invalidate = dlsym(RTLD_DEFAULT, "eVideoInvalidateCache");
	map = dlsym(RTLD_DEFAULT, "eVideoAllocMVA");
	init_mva = dlsym(RTLD_DEFAULT, "eVideoInitMVA");
	deinit_mva = dlsym(RTLD_DEFAULT, "eVideoDeInitMVA");
	if (!create || !release || !allocate || !free_mem || !flush || !invalidate || !map || !init_mva || !deinit_mva) return 4;
	if (init_mva(&mva)) return 16;
	for (uint32_t type = 0; type < 4; type++) {
		struct val_memory mem;
		uint32_t addr = 0;
		/* Match ARM32 stock callers that leave union high words uninitialized. */
		memset(&mem, 0xa5, sizeof(mem));
		mem.type = type;
		mem.size = 0xa5a5a5a500010001ULL;
		mem.alignment = 1024;
		if (allocate(&mem, sizeof(mem))) return 5;
		if (!mem.va || !mem.iova || mem.va > UINT32_MAX || mem.iova > UINT32_MAX ||
		    mem.aligned_iova != mem.iova || (mem.iova & 1023)) return 6;
		memset((void *)(uintptr_t)mem.va, type + 1, mem.size);
		if (flush(&mem, sizeof(mem), 1) || invalidate(&mem, sizeof(mem), 1)) return 7;
		if (((uint8_t *)(uintptr_t)mem.va)[65536] != type + 1) return 8;
		if (map(mva, mem.va + 64, &addr, 4096, NULL) || addr != mem.iova + 64) return 9;
		if (!map(mva, 1, &addr, 4096, NULL)) return 10;
		if (free_mem(&mem, sizeof(mem))) return 11;
		if (!free_mem(&mem, sizeof(mem))) return 12;
		printf("VAL_MEMORY type=%u mapping/cache/free passed\n", type);
	}
	for (unsigned int n = 0; n < 32; n++) {
		instance = NULL;
		int rc = create(&instance, 0x100);
		if (rc || !instance) { printf("CREATE_FAIL iteration=%u rc=%d\n", n, rc); return 13; }
		if (*(uint32_t *)instance != 0x100) return 14;
		rc = release(instance);
		if (rc) { printf("RELEASE_FAIL iteration=%u rc=%d\n", n, rc); return 15; }
	}
	puts("STOCK_H264_CREATE_RELEASE=32 (real stock library; no init or decode job)");
	if (deinit_mva(mva)) return 17;
	dlclose(lib);
	return 0;
}
