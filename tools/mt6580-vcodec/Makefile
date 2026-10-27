# SPDX-License-Identifier: GPL-2.0-only
# Example: make CC=/absolute/path/to/armv7a-linux-androideabi28-clang
CFLAGS += -O2 -Wall -Wextra -Werror
all: libval-buffer-bridge.so buffer-check stock-create-check
libval-buffer-bridge.so: val-buffer-bridge.c ../../include/uapi/linux/mt6580-vcodec.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@
buffer-check: buffer-check.c ../../include/uapi/linux/mt6580-vcodec.h
	$(CC) $(CFLAGS) $< -o $@
stock-create-check: stock-create-check.c
	$(CC) $(CFLAGS) $< -ldl -o $@
clean:
	rm -f libval-buffer-bridge.so buffer-check stock-create-check
.PHONY: all clean
