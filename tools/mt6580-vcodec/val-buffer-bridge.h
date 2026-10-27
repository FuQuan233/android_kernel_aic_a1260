/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6580_VAL_BUFFER_BRIDGE_H
#define MT6580_VAL_BUFFER_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
int mt6580_bridge_fd(void);
int mt6580_bridge_buffer(uintptr_t va, size_t bytes, uint32_t *id,
			 uint32_t *offset, int require_coherent);
int mt6580_bridge_sync(int cpu);
#endif
