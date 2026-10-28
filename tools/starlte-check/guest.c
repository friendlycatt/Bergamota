/* Integration test, not a replacement interrupt driver. See README.md. */
#include "compat.h"
#include <pexpert/arm/protos.h>
#include <arm64/sweet_early.h>

#define RSR(r) __builtin_arm_rsr64(r)
#define WSR(r, v) __builtin_arm_wsr64(r, (uint64_t)(v))
#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define TIMER_TICKS 32

static DTEntry root;
static unsigned int ncpu, version;
static uint64_t freq;
static uint32_t ready[8], timers[8], sgis[8], expected_source[8];
static uint32_t request[8], completed[8], target_cpu[8];
static uint64_t table[2048] __attribute__((aligned(16384)));
unsigned char iso_font[4096];
extern void test_secondary_entry(void);

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--) *d++ = *s++;
	return dst;
}
void *memset(void *dst, int v, size_t n)
{
	uint8_t *d = dst;
	while (n--) *d++ = (uint8_t)v;
	return dst;
}
int memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;
	for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
	return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static void putc_uart(char c)
{
	volatile uint32_t *uart = (volatile uint32_t *)0x09000000;
	while (uart[6] & (1u << 5)) {}
	uart[0] = c;
}
static void print_number(uint64_t n, unsigned int base)
{
	char buf[32];
	unsigned int i = 0;
	do { buf[i++] = "0123456789abcdef"[n % base]; n /= base; } while (n);
	while (i) putc_uart(buf[--i]);
}
static void vprint(const char *fmt, va_list args)
{
	while (*fmt) {
		if (*fmt++ != '%') { putc_uart(fmt[-1]); continue; }
		bool wide = false;
		while (*fmt == 'l') { wide = true; fmt++; }
		char c = *fmt++;
		if (c == 's') { const char *s = va_arg(args, const char *); while (*s) putc_uart(*s++); }
		else if (c == 'u' || c == 'x') print_number(wide ? va_arg(args, uint64_t) : va_arg(args, unsigned int), c == 'x' ? 16 : 10);
		else putc_uart(c);
	}
}
void kprintf(const char *fmt, ...) { va_list args; va_start(args, fmt); vprint(fmt, args); va_end(args); }
void panic(const char *fmt, ...)
{
	__asm__ volatile("msr daifset, #0xf");
	kprintf("FAIL: ");
	va_list args; va_start(args, fmt); vprint(fmt, args); va_end(args);
	kprintf("\n");
	for (;;) __asm__ volatile("wfe");
}
void _doprnt(const char *fmt, va_list *args, void (*out)(char), int radix)
{
	(void)args; (void)radix;
	while (*fmt) out(*fmt++);
}

static uint32_t u32(const void *p) { uint32_t n; memcpy(&n, p, 4); return n; }
static uint64_t u64(const void *p) { uint64_t n; memcpy(&n, p, 8); return n; }
static const uint8_t *properties_end(DTEntry node)
{
	const uint8_t *p = node + 8;
	for (uint32_t i = 0; i < u32(node); i++) p += 36 + ((u32(p + 32) + 3) & ~3u);
	return p;
}
static DTEntry subtree_end(DTEntry node)
{
	DTEntry p = properties_end(node);
	for (uint32_t i = 0; i < u32(node + 4); i++) p = subtree_end(p);
	return p;
}
int SecureDTGetProperty(DTEntry node, const char *key, const void **out, unsigned int *size)
{
	const uint8_t *p = node + 8;
	for (uint32_t i = 0; i < u32(node); i++) {
		uint32_t n = u32(p + 32);
		if (memcmp(p, key, strlen(key) + 1) == 0) { *out = p + 36; *size = n; return 0; }
		p += 36 + ((n + 3) & ~3u);
	}
	return -1;
}
int SecureDTLookupEntry(DTEntry node, const char *path, DTEntry *out)
{
	if (!node) node = root;
	while (*path == '/') path++;
	if (!*path) { *out = node; return 0; }
	size_t len = 0; while (path[len] && path[len] != '/') len++;
	DTEntry child = properties_end(node);
	for (uint32_t i = 0; i < u32(node + 4); i++, child = subtree_end(child)) {
		const void *name; unsigned int size;
		if (!SecureDTGetProperty(child, "name", &name, &size) && size == len + 1 && !memcmp(name, path, len))
			return SecureDTLookupEntry(child, path + len, out);
	}
	return -1;
}
static const void *prop(const char *path, const char *key)
{
	DTEntry node; const void *p; unsigned int size;
	if (SecureDTLookupEntry(NULL, path, &node) || SecureDTGetProperty(node, key, &p, &size)) panic("missing ADT property");
	return p;
}
vm_offset_t pe_arm_get_soc_base_phys(void) { return u64((const uint8_t *)prop("/arm-io", "ranges") + 8); }
vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t size) { (void)size; return pa; }
/* Deliberately permute logical IDs: an SGIR implementation using 1<<logical fails. */
unsigned int ml_get_cpu_number(uint32_t mpidr) { return mpidr == 0 ? 0 : ncpu - mpidr; }
unsigned int ml_get_cpu_number_local(void) { return ml_get_cpu_number((uint32_t)RSR("MPIDR_EL1") & 0xffff); }
uint64_t mach_absolute_time(void) { return RSR("CNTVCT_EL0"); }
void nanoseconds_to_deadline(uint64_t ns, uint64_t *out) { *out = mach_absolute_time() + ns * freq / 1000000000; }

static void enable_mmu(void)
{
	WSR("MAIR_EL1", 0x44ff00);
	WSR("TCR_EL1", 28ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (2ULL << 14) |
	    (28ULL << 16) | (1ULL << 23) | (1ULL << 30) | (1ULL << 32));
	WSR("TTBR0_EL1", (uintptr_t)table);
	__asm__ volatile("isb; tlbi vmalle1; dsb sy; isb" ::: "memory");
	WSR("SCTLR_EL1", (RSR("SCTLR_EL1") & ~((1ULL << 1) | (1ULL << 19))) | 0x1005);
	__asm__ volatile("isb" ::: "memory");
}
static void wait_count(uint32_t *p, uint32_t want)
{
	uint64_t deadline = mach_absolute_time() + freq * 2;
	while (LOAD(p) < want) if (mach_absolute_time() > deadline) panic("timeout: got=%u want=%u", LOAD(p), want);
}
static void cpu_init(unsigned int physical)
{
	pe_init_fiq();
	uint32_t id;
	(void)sweet_gic_ack_irq(&id);
	if (id < 1020 || id > 1023) panic("expected spurious IRQ, got %u", id);
	STORE(&expected_source[physical], physical);
	sweet_gic_send_ipi(ml_get_cpu_number(physical), physical);
	WSR("CNTV_TVAL_EL0", freq / 1000);
	WSR("CNTV_CTL_EL0", 1);
	__asm__ volatile("isb; msr daifclr, #2" ::: "memory");
	wait_count(&sgis[physical], 1);
	STORE(&ready[physical], 1);
}
void test_exception(unsigned int slot)
{
	if (slot != 5) panic("exception slot=%u ESR=%llx ELR=%llx FAR=%llx", slot, RSR("ESR_EL1"), RSR("ELR_EL1"), RSR("FAR_EL1"));
	unsigned int physical = RSR("MPIDR_EL1") & 0xffff;
	uint32_t id, token = sweet_gic_ack_irq(&id);
	if (id >= 1020 && id <= 1023) return;
	if (id == sweet_vtimer_intid) {
		uint32_t count = LOAD(&timers[physical]) + 1;
		if (count < TIMER_TICKS) WSR("CNTV_TVAL_EL0", freq / 1000);
		else WSR("CNTV_CTL_EL0", 0);
		__asm__ volatile("isb" ::: "memory");
		sweet_gic_eoi_irq(token);
		STORE(&timers[physical], count);
	} else if (id == SWEET_IPI_SGI) {
		if (version == 2 && ((token >> 10) & 7) != LOAD(&expected_source[physical])) panic("wrong GICv2 SGI source");
		sweet_gic_eoi_irq(token);
		__atomic_add_fetch(&sgis[physical], 1, __ATOMIC_RELEASE);
	} else panic("unexpected IRQ %u", id);
}
void test_secondary(unsigned int physical)
{
	enable_mmu();
	cpu_init(physical);
	for (;;) {
		uint32_t seq = LOAD(&request[physical]);
		if (seq != LOAD(&completed[physical])) {
			unsigned int target = LOAD(&target_cpu[physical]);
			sweet_gic_send_ipi(ml_get_cpu_number(target), target);
			STORE(&completed[physical], seq);
		}
		__asm__ volatile("wfe");
	}
}
void test_main(const uint8_t *ba)
{
	root = (DTEntry)(uintptr_t)u64(ba + 96);
	DTEntry cpus;
	if (SecureDTLookupEntry(NULL, "/cpus", &cpus)) panic("no CPUs");
	ncpu = u32(cpus + 4);
	version = ((const char *)prop("/arm-io/gic", "compatible"))[4] == 'g' ? 3 : 2;
	freq = RSR("CNTFRQ_EL0");
	uint64_t fb = u64(ba + 40), fb_block = fb & ~0x1ffffffULL;
	for (unsigned int i = 0; i < 2048; i++) {
		uint64_t pa = (uint64_t)i << 25;
		uint64_t flags = (1ULL << 10) | (1ULL << 53) | (1ULL << 54);
		if (pa >= 0x40000000 && pa < 0x140000000) flags = (1ULL << 2) | (3ULL << 8) | (1ULL << 10) | (1ULL << 54);
		if (pa == fb_block) flags = (2ULL << 2) | (2ULL << 8) | (1ULL << 10) | (1ULL << 53) | (1ULL << 54);
		table[i] = pa | flags | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	enable_mmu();
	kprintf("TEST GICv%u CPUs=%u EL=%llu\n", version, ncpu, RSR("CurrentEL") >> 2);
	/* Compare the boot ABI tail against the payload's RAM size. */
	if (u64(ba + 1144) != u64(ba + 24)) panic("XNU26 boot_args tail mismatch");
	sweet_early_set_video(fb, u64(ba + 64), u64(ba + 72), u64(ba + 56));
	if (sweet_early_framebuffer_block() != fb_block) panic("wrong framebuffer block");
	sweet_early_set_cmdline("sweetearlyfb=1");
	sweet_early_mark(1, "QEMU framebuffer");
	/* Invalid geometry must not fall back to a hardcoded Qualcomm address. */
	sweet_early_set_video(fb, 1440, 2960, 4);
	if (sweet_early_framebuffer_block()) panic("accepted invalid framebuffer");
	cpu_init(0);
	for (unsigned int i = 1; i < ncpu; i++) {
		int32_t rc = sweet_psci_cpu_on(i, (uintptr_t)test_secondary_entry, i);
		if (rc) panic("PSCI CPU_ON failed: %x", (uint32_t)rc);
		wait_count(&ready[i], 1);
	}
	for (unsigned int src = 0; src < ncpu; src++) {
		for (unsigned int dst = 0; dst < ncpu; dst++) {
			for (unsigned int round = 0; round < 3; round++) {
				uint32_t want = LOAD(&sgis[dst]) + 1;
				STORE(&expected_source[dst], src);
				if (src == 0) sweet_gic_send_ipi(ml_get_cpu_number(dst), dst);
				else {
					STORE(&target_cpu[src], dst);
					STORE(&request[src], LOAD(&request[src]) + 1);
					__asm__ volatile("dsb ishst; sev" ::: "memory");
				}
				wait_count(&sgis[dst], want);
				if (src) wait_count(&completed[src], LOAD(&request[src]));
			}
		}
	}
	unsigned int total = 0;
	for (unsigned int i = 0; i < ncpu; i++) { wait_count(&timers[i], TIMER_TICKS); total += LOAD(&sgis[i]); }
	kprintf("PASS GICv%u CPUs=%u timers=%u/cpu SGIs=%u (all-to-all, permuted logical IDs)\n", version, ncpu, TIMER_TICKS, total);
	for (;;) __asm__ volatile("wfe");
}
