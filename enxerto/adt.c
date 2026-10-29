/* Bounded Apple Device Tree handoff editing. ADT words are little-endian;
 * values have only four-byte alignment, including pairs of u64s.
 */
#include "adt.h"

static int equal(uintptr_t p, uint64_t len, const char *s)
{
	uint64_t i = 0;
	while (i < len && s[i] && *(const char *)(p + i) == s[i])
		i++;
	return i < len && !s[i] && !*(const char *)(p + i);
}

static int compatible(uintptr_t p, uint64_t len, const char *s)
{
	int found = 0;
	while (len) {
		found |= equal(p, len, s);
		uint64_t n = 0;
		while (n < len && *(const char *)(p + n))
			n++;
		if (n == len)
			return 0;
		p += n + 1;
		len -= n + 1;
	}
	return found;
}

static uint32_t word(uintptr_t p)
{
	return *(const uint32_t *)p;
}

static void put64(uintptr_t p, uint64_t v)
{
	*(uint32_t *)p = (uint32_t)v;
	*(uint32_t *)(p + 4) = (uint32_t)(v >> 32);
}

int adt_ufs_dma(uintptr_t tree, uint64_t bytes, uint64_t image_lo, uint64_t image_hi,
	       uint64_t pool, uint64_t pool_size, uint64_t ram, uint64_t ram_size)
{
	if (!tree || (tree & 3) || bytes > UINTPTR_MAX - tree)
		return -1;
	uintptr_t p = tree, end = tree + bytes, dma = 0, image = 0;
	uint32_t remaining[32];
	remaining[0] = 1;
	unsigned depth = 0;
	int starlte = 0, found = 0;
	for (;;) {
		if (!remaining[depth]) {
			if (!depth)
				break;
			depth--;
			continue;
		}
		remaining[depth]--;
		if (end - p < 8)
			return -1;
		uint32_t props = word(p), children = word(p + 4);
		p += 8;
		int ufs = 0, samsung = 0;
		uintptr_t dp = 0, ip = 0;
		for (uint32_t i = 0; i < props; i++) {
			if (end - p < 36)
				return -1;
			uint64_t len = word(p + 32) & 0x7fffffff;
			uint64_t padded = (len + 3) & ~3ULL;
			uintptr_t v = p + 36;
			if (padded > end - v)
				return -1;
			if (equal(p, 32, "name"))
				ufs = equal(v, len, "sweet-ufs");
			if (!depth && equal(p, 32, "compatible"))
				starlte = compatible(v, len, "samsung,starlte");
			if (depth == 1 && equal(p, 32, "compatible"))
				samsung = compatible(v, len, "samsung,exynos9810-ufs");
			if (depth == 1 && equal(p, 32, "dma-pool")) {
				if (dp || len != 16)
					return -1;
				dp = v;
			}
			if (depth == 1 && equal(p, 32, "loader-image")) {
				if (ip || len != 16)
					return -1;
				ip = v;
			}
			p = v + padded;
		}
		if (depth == 1 && ufs && samsung) {
			if (found++ || !dp || !ip)
				return -1;
			dma = dp;
			image = ip;
		}
		if (children) {
			if (++depth == 32)
				return -1;
			remaining[depth] = children;
		}
	}
	if (p != end)
		return -1;
	if (!found)
		return 0;
	/* No guessed low-RAM address: the whole arena must be in our Image,
	 * below 4 GiB, page aligned and outside XNU's managed bank.
	 */
	if (!starlte || !image_lo || image_hi <= image_lo || image_hi > (1ULL << 32) ||
	    (pool & 0x3fff) || pool_size != 0x104000 || pool < image_lo ||
	    pool > image_hi || pool_size > image_hi - pool ||
	    !ram_size || ram_size > UINT64_MAX - ram ||
	    (image_lo < ram + ram_size && ram < image_hi))
		return -1;
	/* Reject an already-filled or inconsistent payload. No changes on error. */
	for (unsigned i = 0; i < 16; i += 4)
		if (word(dma + i) || word(image + i))
			return -1;
	put64(dma, pool);
	put64(dma + 8, pool_size);
	put64(image, image_lo);
	put64(image + 8, image_hi - image_lo);
	return 1;
}
