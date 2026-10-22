/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libfdt.h>

#include "fdt_bootloader_initrd.h"

union tree {
	uint64_t alignment;
	unsigned char data[8192];
};

static void create_tree(union tree *tree, int loader)
{
	int node;
	fdt32_t reg[] = { cpu_to_fdt32(0), cpu_to_fdt32(0x83f00000),
			 cpu_to_fdt32(0), cpu_to_fdt32(0x100000) };

	memset(tree, 0, sizeof(*tree));
	assert(!fdt_create_empty_tree(tree->data, sizeof(tree->data)));
	node = fdt_add_subnode(tree->data, 0, "chosen");
	assert(node >= 0);
	assert(!fdt_setprop_string(tree->data, node, "bootargs", loader ?
		"androidboot.hardware=mt6580 vmalloc=496M" :
		"androidboot.hardware=mt6580 maxcpus=1"));
	node = fdt_add_subnode(tree->data, 0, "reserved-memory");
	assert(node >= 0);
	assert(!fdt_setprop(tree->data, node, "reg", reg, sizeof(reg)));
}

static void location(union tree *tree, int cells, uint64_t start, uint64_t end)
{
	int node = fdt_path_offset(tree->data, "/chosen");

	if (cells == 1) {
		assert(!fdt_setprop_u32(tree->data, node, "linux,initrd-start", start));
		assert(!fdt_setprop_u32(tree->data, node, "linux,initrd-end", end));
	} else {
		assert(!fdt_setprop_u64(tree->data, node, "linux,initrd-start", start));
		assert(!fdt_setprop_u64(tree->data, node, "linux,initrd-end", end));
	}
}

static uint64_t address(const void *tree, const char *name, int cells)
{
	int len, node = fdt_path_offset(tree, "/chosen");
	const fdt32_t *prop = fdt_getprop(tree, node, name, &len);
	uint64_t value = 0;
	int i;

	assert(prop && len == cells * 4);
	for (i = 0; i < cells; ++i)
		value = (value << 32) | fdt32_ld(prop + i);
	return value;
}

static void successful_copy(int cells)
{
	union tree loader, target;
	unsigned char reserved[16];
	int len, node;
	const void *prop;
	uint64_t start = cells == 2 ? 0x184000000ULL : 0x84000000;
	uint64_t end = start + 0x123456;

	create_tree(&loader, 1);
	create_tree(&target, 0);
	location(&loader, cells, start, end);
	node = fdt_path_offset(target.data, "/reserved-memory");
	prop = fdt_getprop(target.data, node, "reg", &len);
	assert(prop && len == sizeof(reserved));
	memcpy(reserved, prop, sizeof(reserved));
	assert(!fdt_copy_bootloader_initrd(loader.data, target.data,
					 sizeof(target.data)));
	assert(address(target.data, "linux,initrd-start", cells) == start);
	assert(address(target.data, "linux,initrd-end", cells) == end);
	node = fdt_path_offset(target.data, "/chosen");
	prop = fdt_getprop(target.data, node, "bootargs", &len);
	assert(prop && !strcmp(prop, "androidboot.hardware=mt6580 maxcpus=1"));
	node = fdt_path_offset(target.data, "/reserved-memory");
	prop = fdt_getprop(target.data, node, "reg", &len);
	assert(prop && len == sizeof(reserved) && !memcmp(prop, reserved, len));
}

int main(void)
{
	union tree loader, target, before;
	int node;

	successful_copy(1);
	successful_copy(2);
	create_tree(&loader, 1);
	create_tree(&target, 0);
	before = target;
	assert(!fdt_copy_bootloader_initrd(loader.data, target.data, sizeof(target.data)));
	assert(!memcmp(&target, &before, sizeof(target)));
	assert(!fdt_copy_bootloader_initrd(target.data, target.data, sizeof(target.data)));
	node = fdt_path_offset(loader.data, "/chosen");
	assert(!fdt_setprop_u32(loader.data, node, "linux,initrd-start", 0x84000000));
	assert(fdt_copy_bootloader_initrd(loader.data, target.data,
					 sizeof(target.data)) == -FDT_ERR_BADVALUE);
	assert(!memcmp(&target, &before, sizeof(target)));
	location(&loader, 1, 0x84123456, 0x84000000);
	assert(fdt_copy_bootloader_initrd(loader.data, target.data,
					 sizeof(target.data)) == -FDT_ERR_BADVALUE);
	location(&loader, 1, 0x84000000, 0x84123456);
	assert(fdt_copy_bootloader_initrd(loader.data, target.data, 16) < 0);
	loader.data[0] = 0;
	assert(fdt_copy_bootloader_initrd(loader.data, target.data, sizeof(target.data)) < 0);
	puts("bootloader initrd: 32/64-bit addresses, preserved bootargs/reservations, absent and invalid metadata PASS");
	return 0;
}
