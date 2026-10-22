/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FDT_BOOTLOADER_INITRD_H
#define FDT_BOOTLOADER_INITRD_H

#include <libfdt.h>

/* Snapshot the values before expanding the destination tree. */
static int fdt_copy_bootloader_initrd(const void *bootloader, void *fdt,
				    int total_space)
{
	unsigned char start_value[8], end_value[8];
	const void *start, *end;
	int chosen, start_len, end_len, ret;

	if (bootloader == fdt)
		return 0;
	ret = fdt_check_header(bootloader);
	if (ret)
		return ret;
	chosen = fdt_path_offset(bootloader, "/chosen");
	if (chosen == -FDT_ERR_NOTFOUND)
		return 0;
	if (chosen < 0)
		return chosen;
	start = fdt_getprop(bootloader, chosen, "linux,initrd-start", &start_len);
	end = fdt_getprop(bootloader, chosen, "linux,initrd-end", &end_len);
	if (!start && !end && start_len == -FDT_ERR_NOTFOUND &&
	    end_len == -FDT_ERR_NOTFOUND)
		return 0;
	if (!start || !end || (start_len != 4 && start_len != 8) ||
	    end_len != start_len)
		return -FDT_ERR_BADVALUE;
	memcpy(start_value, start, start_len);
	memcpy(end_value, end, end_len);
	if (memcmp(end_value, start_value, start_len) <= 0)
		return -FDT_ERR_BADVALUE;

	ret = fdt_open_into(fdt, fdt, total_space);
	if (ret)
		return ret;
	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen == -FDT_ERR_NOTFOUND)
		chosen = fdt_add_subnode(fdt, 0, "chosen");
	if (chosen < 0)
		return chosen;
	ret = fdt_setprop(fdt, chosen, "linux,initrd-start", start_value, start_len);
	if (ret)
		return ret;
	ret = fdt_setprop(fdt, chosen, "linux,initrd-end", end_value, end_len);
	if (ret)
		return ret;
	return fdt_pack(fdt);
}

#endif
