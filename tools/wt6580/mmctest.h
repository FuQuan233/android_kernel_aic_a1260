/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded read-only test. O_DIRECT ensures each pass reaches the eMMC. */
static unsigned char mmc_read_buf[65536] __attribute__((aligned(4096)));

static unsigned int mmc_crc32(unsigned int crc, const unsigned char *data,
			      unsigned int size)
{
	unsigned int i;

	while (size--) {
		crc ^= *data++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
	}
	return crc;
}

static void mmc_read_test(void)
{
	unsigned int crc[2], pass, block, i, n;
	const char *prefix = "\n[WT6580 eMMC read-only: 2 x 1 MiB O_DIRECT CRC32=";
	char reply[160];
	long fd, ret;

	for (pass = 0; pass < 2; pass++) {
		/* ARM O_DIRECT=0200000, O_LARGEFILE=0400000, access O_RDONLY. */
		fd = open_file("/dev/mmcblk0", 0200000 | 0400000);
		if (fd < 0) {
			usb_reply("\n[WT6580 eMMC read-only open failed]\n");
			return;
		}
		crc[pass] = ~0U;
		for (block = 0; block < 16; block++) {
			if (watchdog >= 0)
				write_all(watchdog, "K", 1);
			do {
				ret = syscall5(3, fd, (long)mmc_read_buf,
					       sizeof(mmc_read_buf), 0, 0);
			} while (ret == -4);
			if (ret != sizeof(mmc_read_buf)) {
				syscall5(6, fd, 0, 0, 0, 0);
				usb_reply("\n[WT6580 eMMC read-only I/O failed]\n");
				return;
			}
			crc[pass] = mmc_crc32(crc[pass], mmc_read_buf, sizeof(mmc_read_buf));
		}
		syscall5(6, fd, 0, 0, 0, 0);
		crc[pass] ^= ~0U;
	}
	n = 0;
	while (*prefix)
		reply[n++] = *prefix++;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < 8; i++)
			reply[n++] = "0123456789abcdef"[(crc[pass] >> (28 - 4 * i)) & 15];
		reply[n++] = ' ';
	}
	prefix = crc[0] == crc[1] ? "MATCH]\n" : "MISMATCH]\n";
	while (*prefix)
		reply[n++] = *prefix++;
	reply[n] = 0;
	usb_reply(reply);
}
