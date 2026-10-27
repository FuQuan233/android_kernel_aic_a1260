# SPDX-License-Identifier: GPL-2.0-only
# Example: make CC=/absolute/path/to/armv7a-linux-androideabi28-clang
CFLAGS += -O2 -Wall -Wextra -Werror
all: libval-buffer-bridge.so libval-decode-bridge.so buffer-check job-check stock-create-check stock-decode-probe
libval-buffer-bridge.so: val-buffer-bridge.c ../../include/uapi/linux/mt6580-vcodec.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@
libval-decode-bridge.so: val-buffer-bridge.c legacy-control-bridge.c val-buffer-bridge.h ../../include/uapi/linux/mt6580-vcodec.h
	$(CC) $(CFLAGS) -fPIC -shared val-buffer-bridge.c legacy-control-bridge.c -ldl -o $@
stock-decode-probe: stock-decode-probe.c
	$(CC) $(CFLAGS) $< -ldl -o $@
job-check: job-check.c ../../include/uapi/linux/mt6580-vcodec.h
	$(CC) $(CFLAGS) -D_LARGEFILE64_SOURCE $< -o $@
buffer-check: buffer-check.c ../../include/uapi/linux/mt6580-vcodec.h
	$(CC) $(CFLAGS) $< -o $@
stock-create-check: stock-create-check.c
	$(CC) $(CFLAGS) $< -ldl -o $@
clean:
	rm -f libval-buffer-bridge.so libval-decode-bridge.so buffer-check job-check stock-create-check stock-decode-probe
.PHONY: all clean
