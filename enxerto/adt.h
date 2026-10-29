#pragma once
#include <stdint.h>

/* Fill the starlte UFS placeholders in the copied ADT. Returns 1 when filled,
 * 0 when absent (including QEMU/old payloads), -1 on an invalid handoff.
 * image_hi is exclusive; the pool belongs to the loaded Enxerto Image.
 */
int adt_ufs_dma(uintptr_t tree, uint64_t bytes, uint64_t image_lo, uint64_t image_hi,
	       uint64_t pool, uint64_t pool_size, uint64_t ram, uint64_t ram_size);
