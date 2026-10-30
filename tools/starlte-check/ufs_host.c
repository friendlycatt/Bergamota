/* Byte-level UFSHCI/Samsung device model and host kernel-service adapters.
 *
 * The model decodes DMA bytes using the UFSHCI layout, not the driver's C
 * structures. Its Samsung rules come from the pinned ufshcd/ufs-exynos/FMP
 * sources documented in docs/STARLTE.md. This tests the software protocol;
 * it does not emulate Samsung clocks, PHY, secure firmware or real DMA.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <zlib.h>

static const char *case_name;
static unsigned cases;
#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d (%s): %s\n", __FILE__, __LINE__, case_name, #x); \
	abort(); \
} } while (0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define __unused __attribute__((unused))
#define TRUE 1
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_USEC 1000ull
#define PAGE_MASK 0x3fffull
#define DEV_BSIZE 512u
#define FWRITE 2
#define B_READ 1u
#define B_FUA 2u
#define THREAD_UNINT 0
#define THREAD_WAITING 1
#define THREAD_CONTINUE_NULL NULL
#define TIMEOUT_URGENCY_SYS_CRITICAL 0
#define KERN_SUCCESS 0
#define KMA_DATA 1
#define KMA_ZERO 2
#define KMA_PERMANENT 4
#define VM_KERN_MEMORY_IOKIT 0
#define DEVFS_BLOCK 0
#define DEVFS_CHAR 1
#define UID_ROOT 0
#define GID_OPERATOR 5
#define kSuccess 0
#define kernel_map NULL
#define kernel_pmap NULL
#define ptoa_64(pn) ((pn) << 14)
#define LCK_GRP_DECLARE(name, label) int name __unused
#define LCK_MTX_DECLARE(name, group) int name
#define __builtin_arm_dsb(option) model_barrier()

typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef uint64_t addr64_t;
typedef uint64_t ppnum_t;
typedef int kern_return_t;
typedef void *event_t;
typedef struct test_node *DTEntry;
struct proc;
enum {
	DKIOCGETBLOCKSIZE = 1, DKIOCSETBLOCKSIZE, DKIOCGETPHYSICALBLOCKSIZE,
	DKIOCGETBLOCKCOUNT, DKIOCISWRITABLE, DKIOCSYNCHRONIZE, DKIOCSYNCHRONIZECACHE,
	DKIOCGETMAXBLOCKCOUNTREAD, DKIOCGETMAXBLOCKCOUNTWRITE,
	DKIOCGETMAXSEGMENTCOUNTREAD, DKIOCGETMAXSEGMENTCOUNTWRITE,
};
struct test_buf {
	int64_t blkno;
	uint32_t count, flags, resid;
	char *data;
	int error;
	bool mapped, done, fail_map;
};
typedef struct test_buf *buf_t;
static int64_t buf_blkno(buf_t b) { return b->blkno; }
static uint32_t buf_count(buf_t b) { return b->count; }
static uint32_t buf_flags(buf_t b) { return b->flags; }
static uint32_t buf_resid(buf_t b) { return b->resid; }
static void buf_setresid(buf_t b, uint32_t n) { b->resid = n; }
static void buf_seterror(buf_t b, int e) { b->error = e; }
static void buf_biodone(buf_t b) { CHECK(!b->mapped && !b->done); b->done = true; }
static int buf_map(buf_t b, caddr_t *p) {
	if (b->fail_map) { return ENOMEM; }
	CHECK(!b->mapped);
	b->mapped = true;
	*p = b->data;
	return 0;
}
static void buf_unmap(buf_t b) { CHECK(b->mapped); b->mapped = false; }
static void lck_mtx_lock(int *l) { CHECK(*l == 0); *l = 1; }
static void lck_mtx_unlock(int *l) { CHECK(*l == 1); *l = 0; }

/* These declarations are the service boundary of ufs-production.inc. */
static uint32_t model_rd(uint32_t, bool);
static uint32_t model_unipro_rd(uint32_t);
static void model_wr(uint32_t, uint32_t, bool);
static uint64_t model_smc(void);
static void model_barrier(void);
static int model_printf(const char *, ...);
static void sweet_dma_sync_va(const void *, uint32_t);
static void flush_dcache64(addr64_t, uint32_t, int);
static void clean_dcache64(addr64_t, uint32_t, int);
static void nanoseconds_to_absolutetime(uint64_t, uint64_t *);
static uint64_t mach_absolute_time(void);
static void delay(int);
static bool ml_get_interrupts_enabled(void) { return true; }
static bool ml_at_interrupt_context(void) { return false; }
static int get_preemption_level(void) { return 0; }
static int assert_wait_deadline_with_leeway(event_t, int, int, uint64_t, uint64_t);
static void assert_wait(event_t, int);
static void thread_block(void *);
static bool sweet_board_is(const char *);
static bool sweet_board_qualcomm(void);
static int SecureDTLookupEntry(DTEntry, const char *, DTEntry *);
static int SecureDTGetProperty(DTEntry, const char *, const void **, unsigned *);
static bool PE_parse_boot_argn(const char *, void *, unsigned);
static vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
static kern_return_t kmem_alloc_contig(void *, vm_offset_t *, vm_size_t, uint64_t,
    uint64_t, uint64_t, int, int);
static ppnum_t pmap_find_phys(void *, addr64_t);
static int bdevsw_add(int, const void *);
static int cdevsw_add_with_bdev(int, const void *, int);
static void *devfs_make_node(dev_t, int, int, int, int, const char *);
static const int sweet_ufs_bdevsw, sweet_ufs_cdevsw;
#define strlcpy host_strlcpy
static size_t host_strlcpy(char *to, const char *from, size_t size) {
	size_t n = strlen(from);
	if (size) { memcpy(to, from, MIN(n, size - 1)); to[MIN(n, size - 1)] = 0; }
	return n;
}
static unsigned services;
#define SERVICE(name) static void name(void) { services++; }
SERVICE(sweet_log_start)
SERVICE(sweet_ufs_selftest_start)
SERVICE(sweet_ufs_syncer_start)
SERVICE(sweet_sample_start)
SERVICE(sweet_keys_init)
SERVICE(sweet_reboot_init)
SERVICE(sweet_touch_init)
SERVICE(beryllium_wlan_init)
SERVICE(sweet80211_init)
SERVICE(sweet_usb_probe)
SERVICE(sweet_cpufreq_max)
SERVICE(sweet_charger_start)
SERVICE(sweet_a630_probe)

#define printf model_printf
#include "ufs-production.inc"
#undef printf

/* Deliberately independent of struct utrd/ucd and their layout constants. */
#define MODEL_WINDOW 0x204000u
#define MODEL_DATA (3u << 20)
#define MODEL_IS_ERRORS 0x71884u
struct property {
	const char *name;
	unsigned size;
	uint8_t raw[80] __attribute__((aligned(8)));
};
struct test_node { struct property props[8]; unsigned count; };
struct disk {
	bool present, gpt;
	uint32_t block;
	uint64_t blocks;
	uint8_t header[4096], entries[8192];
	uint8_t *data;
};
enum fault {
	OK, TIMEOUT, BAD_OCS, BAD_TYPE, BAD_LUN, BAD_TAG, BAD_RESPONSE, HOST_ERROR,
	RESIDUAL, UNDERFLOW, OVERFLOW, SENSE, BUSY, FULL, MISSING_DATA, CHANGED_DATA,
	PENDING_ERROR, COMPLETE_ERROR,
	MISSING_RESPONSE,
};
struct model {
	bool exynos, unknown_board, node_present, chosen_present;
	bool hci_mapped, vendor_mapped, pool_mapped;
	bool sticky_dbr, stop_stuck, tm_stop_stuck, stop_race, base_stuck, start_stuck;
	bool stop_readiness_loss, stop_host_error, hce_pending, hce_stuck;
	bool prdt_stuck, nexus_stuck, fail_hci_map, fail_vendor_map, fail_pool_map;
	bool fail_alloc, fail_verify_alloc, expect_hold, waiting_hold;
	bool pending_live, error_sent, uic_read_clear, drop_error_is, complete_on_uic;
	bool corrupt_on_clear, corrupt_on_ack;
	bool trace_boundaries, trace_through_nop, lists_stopped, boundary_fault_sent, boundary_is_read_clear;
	bool boundary_hce_off;
	/* Inject a fresh state after the first layer sample, before any HCE
	 * enable. These read-clear/persistence controls are independent inputs.
	 */
	bool clear_fault, clear_fault_sent, clear_is_read_clear, late_boundary_sent;
	uint32_t clear_is, clear_uic[5], clear_hce, clear_hcs_clear, clear_hcs_set;
	uint32_t clear_dbr, clear_tm_dbr, clear_rsr, clear_tm_rsr;
	uint32_t hce_uic[5], late_boundary_is, late_boundary_uic[5];
	unsigned late_boundary;
	/* Opt-in controller model: HCE invalidates the link independently of
	 * its HCS bits; UIC startup and an actual NOP reply restore transport.
	 * These controls are test inputs, not the production driver's state.
	 */
	bool startup_model, allow_link_setup, unipro_mapped, fail_unipro_map;
	bool link_live, device_ready, drop_dp_on_hce, reset_clocks_on_hce, last_nop;
	bool uic_pending, uic_stuck, uic_error_sent, uic_error_read_clear;
	bool stale_completion_stuck, uic_ack_stuck, uic_drop_hce, freeze_counter;
	uint64_t uic_start, uic_latency, uic_error_delay;
	uint32_t local_attrs[4], tx_lanes, rx_lanes, connected;
	uint32_t link_result, link_arg2, link_cnf, uic_result, uic_fault_is, uic_fault_codes[5];
	uint32_t uic_hcs_clear, uic_busy_reg, uic_ack_error, clock_stuck;
	unsigned uic_fault_at, uic_commands, uic_sets, uic_gets, link_starts, unipro_reads;
	unsigned uic_polls, uic_arg_mask, clock_writes, nops, nop_replies;
	int nop_corrupt_byte;
	struct { uint32_t command, arg1, arg2, arg3; } uic_history[16];
	uint64_t smc_result, now, wake, pa, allocation_pa;
	uint64_t hce_start, hce_latency, hce_completed;
	uint64_t command_start, error_delay, complete_delay;
	uint32_t hce_hcs, hce_is, hce_dbr, hce_tm_dbr, hce_rsr, hce_tm_rsr;
	uint32_t fault_is, fault_uic[5];
	uint32_t boundary_is, boundary_uic[5], boundary_dbr, boundary_tm_dbr, boundary_hcs_clear;
	uint32_t r[128], v[128], size, clear_value, visits;
	unsigned mmio_reads, mmio_writes, vendor_writes, commands, writes, syncs;
	unsigned base_writes, allocations, maps, smcs, barriers, cache_ops, sleeps;
	unsigned hce_enables, hce_echo_reads, hce_init_reads;
	unsigned uic_reads[5], clears, command_acks;
	unsigned pre_uic_samples, nexus_writes, boundary_fault, boundary_last_writes;
	unsigned bdevs, cdevs, nodes, last_prds, last_len, last_op;
	unsigned opcount[256], fault_left, fault_skip;
	int fault_op;
	enum fault fault;
	uint8_t key, asc, ascq;
	uint8_t last_cdb[16];
	uint8_t *allocation, *dma;
	uint64_t pending_pa;
	uint32_t pending_len;
	struct test_node ufs, chosen;
	struct disk disks[8];
	struct { const char *name; uint32_t value; } args[16];
	unsigned nargs;
	char rootname[65];
	char console[131072];
	size_t console_size;
	struct { char kind; bool vendor; uint32_t off, value; } pre_events[4096];
	unsigned pre_event_count;
} m;
static uint8_t fixture_data[2][4096 + 8192 + 4096];
static uint8_t wire[1u << 20];
static jmp_buf hold;

static void model_event(char kind, bool vendor, uint32_t off, uint32_t value)
{
	if (!m.trace_boundaries || m.commands > (m.trace_through_nop ? 1u : 0u)) { return; }
	CHECK(m.pre_event_count < sizeof(m.pre_events) / sizeof(m.pre_events[0]));
	m.pre_events[m.pre_event_count++] = (typeof(m.pre_events[0])){ kind, vendor, off, value };
}

static int model_printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int n = vsnprintf(m.console + m.console_size, sizeof(m.console) - m.console_size, format, args);
	va_end(args);
	CHECK(n >= 0 && (size_t)n < sizeof(m.console) - m.console_size);
	model_event('p', false, 0, n);
	fwrite(m.console + m.console_size, 1, n, stdout);
	m.console_size += n;
	return n;
}

static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t net32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t net64(const uint8_t *p) { return (uint64_t)net32(p) << 32 | net32(p + 4); }
static void put_le32(uint8_t *p, uint32_t v) {
	for (unsigned i = 0; i < 4; i++) { p[i] = v >> (8 * i); }
}
static void put_le64(uint8_t *p, uint64_t v) {
	put_le32(p, v); put_le32(p + 4, v >> 32);
}
static void put_net32(uint8_t *p, uint32_t v) {
	for (unsigned i = 0; i < 4; i++) { p[i] = v >> (24 - 8 * i); }
}
static void put_net64(uint8_t *p, uint64_t v) {
	put_net32(p, v >> 32); put_net32(p + 4, v);
}

static void passed(const char *name) { cases++; printf("PASS UFS: %s\n", name); }

static struct property *property(struct test_node *node, const char *name)
{
	for (unsigned i = 0; i < node->count; i++) {
		if (!strcmp(node->props[i].name, name)) { return &node->props[i]; }
	}
	CHECK(node->count < 8);
	struct property *p = &node->props[node->count++];
	p->name = name;
	return p;
}

static void set_property(struct test_node *node, const char *name, const void *value, unsigned size)
{
	struct property *p = property(node, name);
	CHECK(size <= sizeof(p->raw) - 4);
	p->size = size;
	/* Match the ADT's four-byte alignment, not the host's uint64_t alignment. */
	memcpy(p->raw + 4, value, size);
}

static void set_pair(const char *name, uint64_t a, uint64_t b)
{
	uint8_t v[16];
	put_le64(v, a); put_le64(v + 8, b);
	set_property(&m.ufs, name, v, sizeof(v));
}

static void arg(const char *name, uint32_t value)
{
	CHECK(m.nargs < 16);
	m.args[m.nargs++] = (typeof(m.args[0])){ name, value };
}

static void disk_fixture(unsigned lun, unsigned block)
{
	struct disk *d = &m.disks[lun];
	d->present = d->gpt = true;
	d->block = block;
	d->blocks = 0x10000;
	const uint8_t *f = fixture_data[block == 4096];
	memcpy(d->header, f, block);
	memcpy(d->entries, f + block, 8192);
	if (!d->data) { d->data = malloc(MODEL_DATA); CHECK(d->data); }
	for (unsigned i = 0; i < MODEL_DATA; i++) { d->data[i] = (i * 13 + (i >> 9) + lun * 19) & 255; }
	memcpy(d->data, f + block + 8192, 4096);
}

static void guards(void)
{
	if (!m.allocation) { return; }
	for (unsigned i = 0; i < 0x4000; i++) {
		CHECK(m.allocation[i] == 0xa7);
		CHECK(m.dma[MODEL_WINDOW + i] == 0xa7);
	}
}

static void reset_model(bool exynos, unsigned block)
{
	guards();
	free(m.allocation);
	for (unsigned i = 0; i < 8; i++) { free(m.disks[i].data); }
	memset(&m, 0, sizeof(m));
	memset(&su, 0, sizeof(su));
	memset(&ss, 0, sizeof(ss));
	memset(part_cache, 0, sizeof(part_cache));
	part_cache_n = ufs_fail_logged = services = 0;
	sweet_ufs_lock = 0;
	su.block_size = 4096;
	m.exynos = exynos;
	m.node_present = m.chosen_present = true;
	m.fault_op = -1;
	m.nop_corrupt_byte = -1;
	m.uic_latency = 2000000;
	m.tx_lanes = m.rx_lanes = 2;
	m.connected = 1;
	m.fault_is = 4;
	static const uint32_t uic[] = { 0x80000001, 0x80000002, 0x80000004, 0x80000040, 0x80000001 };
	memcpy(m.fault_uic, uic, sizeof(uic));
	m.allocation_pa = exynos ? 0x81100000ull : 0xb2000000ull;
	CHECK(posix_memalign((void **)&m.allocation, 0x4000, MODEL_WINDOW + 0x8000) == 0);
	memset(m.allocation, 0xa7, MODEL_WINDOW + 0x8000);
	m.dma = m.allocation + 0x4000;
	m.r[0x34 / 4] = exynos ? 0 : 1;
	m.r[0x30 / 4] = exynos ? 0x10f : 3;
	m.hce_latency = 6000000;
	m.hce_hcs = 0x10f;
	m.r[0x00 / 4] = exynos ? 31 : 0x100001f;
	m.r[0x08 / 4] = 0x210;
	m.r[0x50 / 4] = 0x82000000;
	m.r[0x60 / 4] = m.r[0x80 / 4] = 1;
	m.v[0x00 / 4] = 0x8000000c;
	m.v[0x04 / 4] = 12;
	m.v[0x40 / 4] = 0xa5000010;
	set_pair("reg", exynos ? 0x11120000 : 0x1d84000, exynos ? 0x200 : 0x1000);
	set_pair("vendor-reg", 0x11121100, 0x200);
	set_pair("unipro-reg", 0x11110000, 0x8000);
	set_pair("dma-pool", 0x81100000, 0x104000);
	set_pair("loader-image", 0x81000000, 0x300000);
	const char *compat = exynos ? "samsung,exynos9810-ufs" : "qcom,ufshc";
	set_property(&m.ufs, "compatible", compat, strlen(compat) + 1);
	uint64_t ram = 0x880000000, size = 0x80000000;
	set_property(&m.chosen, "dram-base", &ram, 8);
	set_property(&m.chosen, "dram-size", &size, 8);
	disk_fixture(0, block);
}

static uint8_t *dma_at(uint64_t pa, uint64_t len)
{
	CHECK(m.pool_mapped && pa >= m.pa && pa - m.pa <= m.size && len <= m.size - (pa - m.pa));
	if (m.exynos) { CHECK(pa < 0x100000000ull && len <= 0x100000000ull - pa); }
	return m.dma + (pa - m.pa);
}

static void disk_read(unsigned lun, uint64_t lba, uint8_t *dst, uint32_t bytes)
{
	struct disk *d = &m.disks[lun];
	CHECK(d->present && d->block && bytes % d->block == 0);
	CHECK(lba < d->blocks && bytes / d->block <= d->blocks - lba);
	for (uint32_t i = 0; i < bytes; i++) {
		uint64_t pos = lba * d->block + i;
		uint64_t base = 0x100ull * d->block;
		if (d->gpt && pos >= d->block && pos < 2ull * d->block) {
			dst[i] = d->header[pos - d->block];
		} else if (d->gpt && pos >= 2ull * d->block && pos < 2ull * d->block + 8192) {
			dst[i] = d->entries[pos - 2ull * d->block];
		} else if (d->data && pos >= base && pos - base < MODEL_DATA) {
			dst[i] = d->data[pos - base];
		} else {
			dst[i] = (pos ^ (pos >> 13) ^ (lun * 23)) & 255;
		}
	}
}

static void model_command(void)
{
	CHECK(m.r[0x60 / 4] == 1 && m.r[0x58 / 4] == 1);
	CHECK(!m.hce_pending && (m.r[0x34 / 4] & 1));
	if (m.exynos) { CHECK((m.r[0x30 / 4] & 0xff) == 0xf); }
	uint64_t list = m.r[0x50 / 4] | (uint64_t)m.r[0x54 / 4] << 32;
	CHECK(!(list & 1023));
	uint8_t *t = dma_at(list, 32);
	uint64_t cmd_pa = le64(t + 16);
	CHECK(!(cmd_pa & 127));
	uint8_t *cmd = dma_at(cmd_pa, 32);
	bool nop = cmd[0] == 0;
	uint32_t len = net32(cmd + 12), direction = (le32(t) >> 25) & 3;
	CHECK(len <= sizeof(wire));
	CHECK(le32(t) == (0x11000000u | direction << 25));
	CHECK(le32(t + 4) == 0 && le32(t + 8) == 15 && le32(t + 12) == 0);
	CHECK((nop ? m.startup_model : cmd[0] == 1) && cmd[1] == (len ? (direction == 1 ? 0x20 : 0x40) : 0));
	if (nop) {
		CHECK(!len && !direction);
		for (unsigned i = 0; i < 32; i++) { CHECK(!cmd[i]); }
	}
	CHECK(direction == (len ? ((cmd[16] == 0x2a) ? 1 : 2) : 0));
	CHECK(cmd[2] < 8 && cmd[4] == 0 && net32(cmd + 8) == 0);
	unsigned unit = m.exynos ? 1 : 4;
	unsigned resp_offset = le16(t + 26) * unit, resp_size = le16(t + 24) * unit;
	unsigned prdt_offset = le16(t + 30) * unit, entries = le16(t + 28);
	unsigned stride = m.exynos ? 128 : 16;
	if (m.exynos) {
		CHECK(m.smcs == 1 && m.smc_result == 0 && cmd[3] == 0);
		CHECK((m.v[0x40 / 4] & 1) == (nop ? 0u : 1u));
		CHECK((m.v[0] & 31) == 12 && (m.v[1] & 31) == 12);
		CHECK(entries % 128 == 0);
		entries /= 128;
		/* Force each next request to program its own Nexus type. */
		m.v[0x40 / 4] ^= 1u;
	}
	CHECK(resp_offset >= 32 && resp_size >= 52 && prdt_offset >= resp_offset + resp_size);
	uint8_t *resp = dma_at(cmd_pa + resp_offset, resp_size);
	uint8_t *prdt = dma_at(cmd_pa + prdt_offset, entries * stride);
	CHECK(entries <= 64 && (!len == !entries));
	struct { uint64_t pa; uint32_t len; } spans[64];
	uint32_t total = 0;
	for (unsigned i = 0; i < entries; i++) {
		uint8_t *p = prdt + i * stride;
		uint32_t dbc = le32(p + 12);
		spans[i].pa = le64(p);
		spans[i].len = dbc + 1;
		CHECK(dbc <= 0x3ffff && !(spans[i].pa & 3) && !(spans[i].len & 3));
		CHECK(le32(p + 8) == 0);
		for (unsigned j = 16; j < stride; j++) { CHECK(p[j] == 0); }
		CHECK(total <= len && spans[i].len <= len - total);
		uint8_t *data = dma_at(spans[i].pa, spans[i].len);
		/* Descriptors and response must never alias the data transfer. */
		CHECK(spans[i].pa >= cmd_pa + prdt_offset + entries * stride);
		if (direction == 1) { memcpy(wire + total, data, spans[i].len); }
		total += spans[i].len;
	}
	CHECK(total == len);
	m.commands++;
	m.last_nop = nop;
	if (nop) { m.nops++; }
	m.last_prds = entries;
	m.last_len = len;
	m.last_op = cmd[16];
	if (!nop) {
		m.opcount[cmd[16]]++;
		m.visits |= 1u << cmd[2];
	}
	memcpy(m.last_cdb, cmd + 16, 16);
	enum fault fault = OK;
	if (m.fault_left && (m.fault_op < 0 || m.fault_op == (nop ? 256 : cmd[16]))) {
		if (m.fault_skip) { m.fault_skip--; }
		else { fault = m.fault; m.fault_left--; }
	}
	/* HCS can look ready after HCE while no link/UTP session exists. */
	if (m.startup_model && (!m.link_live || (!nop && !m.device_ready))) { fault = TIMEOUT; }
	if (fault == TIMEOUT || fault == PENDING_ERROR) {
		m.pending_pa = entries ? spans[0].pa : cmd_pa + resp_offset;
		m.pending_len = entries ? spans[0].len : 32;
		m.command_start = m.now;
		m.pending_live = fault == PENDING_ERROR;
		return;
	}
	memset(resp, 0, 52);
	resp[0] = nop ? 0x20 : 0x21;
	resp[2] = cmd[2];
	resp[3] = cmd[3];
	put_le32(t + 8, 0);
	struct disk *d = &m.disks[cmd[2]];
	if (nop) {
		CHECK(m.link_live && !entries && !m.opcount[0]);
		m.nop_replies++;
		m.device_ready = true;
		if (m.nop_corrupt_byte >= 0) {
			CHECK(m.nop_corrupt_byte < 32);
			resp[m.nop_corrupt_byte] ^= 1;
		}
	} else if (!d->present || fault == SENSE) {
		resp[7] = 2;
		resp[11] = 20;
		resp[33] = 18;
		resp[34] = 0x70;
		resp[36] = d->present ? m.key : 5;
		resp[41] = 10;
		resp[46] = d->present ? m.asc : 0x25; /* LOGICAL UNIT NOT SUPPORTED */
		resp[47] = d->present ? m.ascq : 0;
	} else if (fault == BUSY || fault == FULL) {
		resp[7] = fault == BUSY ? 8 : 0x28;
	} else {
		switch (cmd[16]) {
		case 0x00: CHECK(len == 0); break;
		case 0x25:
			CHECK(len == 8);
			put_net32(wire, d->blocks - 1 > UINT32_MAX ? UINT32_MAX : d->blocks - 1);
			put_net32(wire + 4, d->block);
			break;
		case 0x9e:
			CHECK(cmd[17] == 0x10 && len == 32 && net32(cmd + 26) == len);
			memset(wire, 0, len);
			put_net64(wire, d->blocks - 1);
			put_net32(wire + 8, d->block);
			break;
		case 0x28:
		case 0x88: {
			uint64_t lba = cmd[16] == 0x28 ? net32(cmd + 18) : net64(cmd + 18);
			uint32_t count = cmd[16] == 0x28 ? (unsigned)cmd[23] << 8 | cmd[24] : net32(cmd + 26);
			CHECK(count && len == (uint64_t)count * d->block);
			disk_read(cmd[2], lba, wire, len);
			break;
		}
		case 0x2a: {
			CHECK(!m.exynos); /* Any Samsung write reaching the model fails the suite. */
			uint64_t lba = net32(cmd + 18);
			uint32_t count = (unsigned)cmd[23] << 8 | cmd[24];
			CHECK(count && len == (uint64_t)count * d->block && lba >= 0x100);
			uint64_t offset = (lba - 0x100) * d->block;
			CHECK(offset <= MODEL_DATA && len <= MODEL_DATA - offset);
			memcpy(d->data + offset, wire, len);
			m.writes++;
			break;
		}
		case 0x35: CHECK(!m.exynos && len == 0); m.syncs++; break;
		default: CHECK(false);
		}
		if (fault == CHANGED_DATA && len) { wire[len - 1] ^= 1; }
		if (direction == 2 && fault != MISSING_DATA) {
			uint32_t off = 0;
			for (unsigned i = 0; i < entries; i++) {
				memcpy(dma_at(spans[i].pa, spans[i].len), wire + off, spans[i].len);
				off += spans[i].len;
			}
		}
	}
	switch (fault) {
	case BAD_OCS: put_le32(t + 8, 2); break;
	case BAD_TYPE: resp[0] = nop ? 0x21 : 0x31; break;
	case MISSING_RESPONSE: memset(resp, 0, 52); break;
	case BAD_LUN: resp[2] ^= 1; break;
	case BAD_TAG: resp[3] ^= 1; break;
	case BAD_RESPONSE: resp[6] = 1; break;
	case HOST_ERROR: m.r[0x20 / 4] |= 0x10000; break;
	case COMPLETE_ERROR:
		m.r[0x20 / 4] |= m.fault_is;
		for (unsigned i = 0; i < 5; i++) { m.r[(0x38 / 4) + i] = m.fault_uic[i]; }
		put_le32(t + 8, 0x1a000000); /* raw reserved bits retained, OCS success is not enough */
		break;
	case RESIDUAL: put_net32(resp + 12, 4); break;
	case UNDERFLOW: resp[1] = 0x20; break;
	case OVERFLOW: resp[1] = 0x40; break;
	default: break;
	}
	m.r[0x20 / 4] |= 1;
	m.r[0x58 / 4] = 0;
}

static void model_enable_progress(void)
{
	if (!m.hce_pending) { return; }
	uint64_t elapsed = m.now - m.hce_start;
	if (elapsed < 1000000) { return; } /* immediate write echo, not initialized */
	m.r[0x34 / 4] = 0;
	if (m.hce_stuck || elapsed < m.hce_latency) { return; }
	m.r[0x34 / 4] = 1;
	m.r[0x30 / 4] = m.hce_hcs;
	m.r[0x20 / 4] |= m.hce_is;
	m.r[0x58 / 4] = m.hce_dbr;
	m.r[0x78 / 4] = m.hce_tm_dbr;
	m.r[0x60 / 4] = m.hce_rsr;
	m.r[0x80 / 4] = m.hce_tm_rsr;
	for (unsigned i = 0; i < 5; i++) {
		if (m.hce_uic[i]) { m.r[0x38 / 4 + i] = m.hce_uic[i]; }
	}
	if (m.startup_model) {
		m.link_live = m.device_ready = false;
		memset(m.local_attrs, 0, sizeof(m.local_attrs));
		if (m.drop_dp_on_hce) { m.r[0x30 / 4] &= ~1u; }
		if (m.reset_clocks_on_hce) {
			m.v[0xb4 / 4] |= 0x10;
			m.v[0xb0 / 4] |= 0x17;
			m.v[0xfc / 4] &= ~1u;
		}
	}
	m.hce_pending = false;
	m.hce_completed = m.now;
}

static void model_pending_complete(void)
{
	uint8_t *resp = m.dma + 0x1200;
	memset(resp, 0, 16);
	resp[0] = m.last_nop ? 0x20 : 0x21;
	if (m.last_nop) { m.nop_replies++; m.device_ready = true; }
	put_le32(m.dma + 8, 0);
	m.r[0x58 / 4] = 0;
	m.r[0x20 / 4] |= 1;
	m.pending_live = false;
}

static void model_command_progress(void)
{
	if (!m.pending_live) { return; }
	uint64_t elapsed = m.now - m.command_start;
	if (!m.error_sent && elapsed >= m.error_delay) {
		m.error_sent = true;
		m.r[0x20 / 4] |= m.fault_is;
		for (unsigned i = 0; i < 5; i++) { m.r[(0x38 / 4) + i] = m.fault_uic[i]; }
		/* Deliberately partial/inconsistent live DMA, not a completed response. */
		put_le32(m.dma + 8, 0x1a00000f);
		static const uint8_t response[16] = { 0x21, 0x40, 7, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
		memcpy(m.dma + 0x1200, response, sizeof(response));
	}
	if (m.complete_delay && elapsed >= m.complete_delay) { model_pending_complete(); }
}

/* Inject after the software's earlier idle/readiness checks, on the next IS
 * observation after list stop or Nexus preparation. This also exposes a
 * driver that re-reads a cleared error instead of retaining its first value.
 */
static void model_boundary_progress(uint32_t off, bool vendor)
{
	if (vendor || off != 0x20 || !m.lists_stopped) { return; }
	if (m.commands && !(m.startup_model && m.commands == m.nops && m.nexus_writes > m.commands)) { return; }
	if (m.nexus_writes) {
		/* Independent raw 512-byte-slot NOP/TUR layout check, even if no
		 * doorbell is ever submitted. No driver structure offsets used.
		 */
		CHECK(le32(m.dma) == 0x11000000 && le32(m.dma + 8) == 15);
		CHECK(le64(m.dma + 16) == m.pa + 0x1000);
		CHECK(le32(m.dma + 24) == 0x02000200 && le32(m.dma + 28) == 0x04000000);
		CHECK((m.dma[0x1000] == 1 || (m.allow_link_setup && m.dma[0x1000] == 0)) &&
		    m.dma[0x1001] == 0 && m.dma[0x1010] == 0);
		for (unsigned i = 0; i < 512; i++) { CHECK(m.dma[0x1200 + i] == 0); }
	}
	unsigned phase = m.nexus_writes ? (m.dma[0x1000] == 0 ? 3u : 2u) : 1u;
	if (m.boundary_fault_sent && m.late_boundary == phase && !m.late_boundary_sent) {
		m.late_boundary_sent = true;
		m.r[0x20 / 4] |= m.late_boundary_is;
		for (unsigned i = 0; i < 5; i++) { m.r[0x38 / 4 + i] = m.late_boundary_uic[i]; }
	}
	if (!m.boundary_fault || m.boundary_fault_sent || m.boundary_fault != phase) { return; }
	m.boundary_fault_sent = true;
	m.r[0x20 / 4] |= m.boundary_is;
	for (unsigned i = 0; i < 5; i++) { m.r[0x38 / 4 + i] = m.boundary_uic[i]; }
	m.r[0x58 / 4] |= m.boundary_dbr;
	m.r[0x78 / 4] |= m.boundary_tm_dbr;
	m.r[0x30 / 4] &= ~m.boundary_hcs_clear;
	if (m.boundary_hce_off) { m.r[0x34 / 4] = 0; }
}

static void model_clear_progress(uint32_t off, bool vendor)
{
	/* The first UECDME read has completed; the next IS observation sees
	 * this independent new state. Never inject before the original sample.
	 */
	if (vendor || off != 0x20 || !m.clear_fault || m.clear_fault_sent ||
	    !m.boundary_fault_sent || m.uic_reads[4] != 1 || m.hce_enables || m.uic_commands || m.commands) { return; }
	m.clear_fault_sent = true;
	m.r[0x20 / 4] |= m.clear_is;
	for (unsigned i = 0; i < 5; i++) { m.r[0x38 / 4 + i] |= m.clear_uic[i]; }
	m.r[0x34 / 4] = m.clear_hce;
	m.r[0x30 / 4] = (m.r[0x30 / 4] & ~m.clear_hcs_clear) | m.clear_hcs_set;
	m.r[0x58 / 4] |= m.clear_dbr;
	m.r[0x78 / 4] |= m.clear_tm_dbr;
	m.r[0x60 / 4] |= m.clear_rsr;
	m.r[0x80 / 4] |= m.clear_tm_rsr;
}

static bool model_clocks_running(void)
{
	return !(m.v[0xb4 / 4] & 0x10) && !(m.v[0xb0 / 4] & 0x17) && (m.v[0xfc / 4] & 1);
}

static void model_uic_progress(void)
{
	if (!m.uic_pending) { return; }
	uint64_t elapsed = m.now - m.uic_start;
	bool fault = m.uic_commands == m.uic_fault_at;
	if (fault && !m.uic_error_sent && elapsed >= m.uic_error_delay) {
		m.uic_error_sent = true;
		m.r[0x20 / 4] |= m.uic_fault_is;
		for (unsigned i = 0; i < 5; i++) { m.r[0x38 / 4 + i] = m.uic_fault_codes[i]; }
	}
	if ((fault && m.uic_stuck) || elapsed < m.uic_latency) { return; }
	const typeof(m.uic_history[0]) *u = &m.uic_history[m.uic_commands - 1];
	uint32_t result = fault ? m.uic_result : 0;
	unsigned attr = u->arg1 >> 16;
	if (u->command == 2) {
		static const unsigned attrs[] = { 0x3000, 0x3001, 0x4021, 0x4020 };
		bool found = false;
		for (unsigned i = 0; i < 4; i++) {
			if (attr == attrs[i]) { if (!result) { m.local_attrs[i] = u->arg3; } found = true; }
		}
		CHECK(found);
	} else if (u->command == 0x16) {
		CHECK(!attr && !u->arg3);
		CHECK(m.local_attrs[0] == 0 && m.local_attrs[1] == 1 && m.local_attrs[2] == 1 && m.local_attrs[3] == 1);
		m.link_cnf = m.link_result;
		result = m.link_arg2; /* Samsung's generic register may disagree. */
		if (!(m.link_cnf & 255)) { m.link_live = true; m.r[0x30 / 4] |= 1; }
		m.r[0x20 / 4] |= 0x100;
	} else {
		CHECK(u->command == 1 && !u->arg3);
		switch (attr) {
		case 0x1561: m.r[0x9c / 4] = m.link_live ? m.tx_lanes : 0; break;
		case 0x1581: m.r[0x9c / 4] = m.link_live ? m.rx_lanes : 0; break;
		case 0x4020: m.r[0x9c / 4] = m.link_live ? m.connected : 0; break;
		default: CHECK(false);
		}
	}
	m.r[0x98 / 4] = result;
	m.r[0x30 / 4] |= 8;
	if (fault) {
		m.r[0x30 / 4] &= ~m.uic_hcs_clear;
		if (m.uic_drop_hce) { m.r[0x34 / 4] = 0; }
		if (m.uic_busy_reg) { m.r[m.uic_busy_reg / 4] = 1; }
	}
	m.r[0x20 / 4] |= 0x400;
	m.uic_pending = false;
}

static uint32_t model_unipro_rd(uint32_t off)
{
	CHECK(m.allow_link_setup && m.unipro_mapped && off == 0x7854);
	m.mmio_reads++;
	m.unipro_reads++;
	return m.link_cnf;
}

static uint32_t model_rd(uint32_t off, bool vendor)
{
	CHECK((vendor ? m.vendor_mapped : m.hci_mapped) && off < 0x200 && !(off & 3));
	m.mmio_reads++;
	if (!vendor && off == 0x34 && m.hce_pending) {
		if (m.now - m.hce_start < 1000000) { m.hce_echo_reads++; }
		else { m.hce_init_reads++; }
	}
	model_enable_progress();
	model_uic_progress();
	model_command_progress();
	model_boundary_progress(off, vendor);
	model_clear_progress(off, vendor);
	uint32_t value = (vendor ? m.v : m.r)[off / 4];
	if (!vendor && off >= 0x38 && off <= 0x48) {
		CHECK(m.exynos); /* No layer-code reads on Qualcomm, even with ufsprobe=1. */
		if (!m.commands) {
			CHECK(m.lists_stopped);
			if (off == 0x38) { CHECK(++m.pre_uic_samples <= 2 || m.allow_link_setup); }
			if (off == 0x48) { m.boundary_last_writes = m.mmio_writes; }
		}
		m.uic_reads[(off - 0x38) / 4]++;
		if (m.uic_read_clear) { m.r[off / 4] = 0; }
		if (off == 0x48 && m.complete_on_uic && m.pending_live) { model_pending_complete(); }
	}
	if (!vendor && off == 0x20 && m.drop_error_is && m.commands) {
		m.r[off / 4] &= ~m.fault_is;
	}
	if (!vendor && off == 0x20 && m.uic_commands && !m.commands) {
		m.uic_polls++;
		if (m.uic_error_read_clear) { m.r[off / 4] &= ~m.uic_fault_is; }
	}
	if (!vendor && off == 0x20 && m.boundary_fault_sent && m.boundary_is_read_clear && m.commands == m.nops) {
		m.r[off / 4] &= ~m.boundary_is;
	}
	if (!vendor && off == 0x20 && m.clear_fault_sent && m.clear_is_read_clear) {
		m.r[off / 4] &= ~m.clear_is;
	}
	model_event('r', vendor, off, value);
	return value;
}

static void model_cleanup_corruption(void)
{
	for (unsigned i = 0; i < 5; i++) {
		CHECK(m.uic_reads[i]); /* Required observations precede either cleanup write. */
		m.r[(0x38 / 4) + i] = 0;
	}
	memset(m.dma, 0x5a, 32);
	memset(m.dma + 0x1200, 0xa5, 16);
}

static void model_wr(uint32_t off, uint32_t value, bool vendor)
{
	CHECK((vendor ? m.vendor_mapped : m.hci_mapped) && off < 0x200 && !(off & 3));
	model_event('w', vendor, off, value);
	m.mmio_writes++;
	if (vendor) {
		CHECK(m.exynos);
		bool clock = off == 0xb4 || off == 0xb0 || off == 0xfc;
		CHECK(off == 0 || off == 4 || off == 0x40 || (m.allow_link_setup && clock));
		m.vendor_writes++;
		if (clock) {
			CHECK(!m.r[0x60 / 4] && !m.r[0x80 / 4] && !m.r[0x58 / 4] && !m.r[0x78 / 4]);
			CHECK(value == (off == 0xfc ? m.v[off / 4] | 1u :
			    m.v[off / 4] & ~(off == 0xb4 ? 0x10u : 0x17u)));
			m.clock_writes++;
			if (m.clock_stuck == off) { return; }
		}
		if (m.prdt_stuck && (off == 0 || off == 4)) { return; }
		if (m.nexus_stuck && off == 0x40) { return; }
		if (off == 0x40) { m.nexus_writes++; }
		m.v[off / 4] = value;
		return;
	}
	CHECK(off == 0x20 || off == 0x24 || off == 0x34 || off == 0x50 ||
	    off == 0x54 || off == 0x58 || off == 0x5c || off == 0x60 || off == 0x80 ||
	    (m.allow_link_setup && off >= 0x90 && off <= 0x9c));
	if (off >= 0x90 && off <= 0x9c) {
		CHECK(m.exynos && !m.hce_pending && (m.r[0x34 / 4] & 1) && !m.uic_pending);
		CHECK(!m.r[0x60 / 4] && !m.r[0x80 / 4] && !m.r[0x58 / 4] && !m.r[0x78 / 4]);
		CHECK(model_clocks_running() && !m.commands && !m.smcs && !m.base_writes);
		m.r[off / 4] = value;
		if (off != 0x90) { m.uic_arg_mask |= 1u << ((off - 0x94) / 4); return; }
		CHECK(m.uic_arg_mask == 7 && !(m.r[0x20 / 4] & 0x400) && (m.r[0x30 / 4] & 8));
		CHECK(m.uic_commands < 16 && (value == 1 || value == 2 || value == 0x16));
		CHECK(!(m.r[0x94 / 4] & 0xffff) && !m.r[0x98 / 4]); /* normal/local, selector 0 */
		m.uic_history[m.uic_commands++] = (typeof(m.uic_history[0])){
		    value, m.r[0x94 / 4], m.r[0x98 / 4], m.r[0x9c / 4] };
		if (value == 1) { m.uic_gets++; }
		if (value == 2) { m.uic_sets++; }
		if (value == 0x16) { CHECK(++m.link_starts == 1 && m.hce_enables == 1); }
		m.uic_pending = true;
		m.uic_error_sent = false;
		m.uic_start = m.now;
		m.uic_arg_mask = 0;
		m.r[0x30 / 4] &= ~8u;
		return;
	}
	if (off == 0x34) {
		CHECK(m.exynos && value == 1 && !m.hce_pending && !(m.r[off / 4] & 1));
		CHECK(!m.r[0x60 / 4] && !m.r[0x80 / 4] && !m.r[0x58 / 4] && !m.r[0x78 / 4]);
		CHECK((m.r[0x30 / 4] & 0xff) == 0xf && !(m.r[0x20 / 4] & MODEL_IS_ERRORS));
		CHECK(!m.smcs && !m.base_writes && !m.commands && ++m.hce_enables == 1);
		m.hce_pending = true; m.hce_start = m.now;
		m.r[off / 4] = 1; /* controller's immediate echo must not pass the enable wait */
		return;
	}
	if (off == 0x20) {
		if (m.commands) {
			m.command_acks++;
			if (m.corrupt_on_ack) { model_cleanup_corruption(); }
		}
		bool completion = (value & m.r[off / 4] & 0x400) != 0;
		bool fault = m.allow_link_setup && m.uic_commands == m.uic_fault_at;
		if (m.stale_completion_stuck || (fault && m.uic_ack_stuck)) { value &= ~0x400u; }
		m.r[off / 4] &= ~value;
		if (completion && fault) { m.r[off / 4] |= m.uic_ack_error; }
		return;
	}
	if (off == 0x5c) {
		m.clears++;
		if (m.corrupt_on_clear) { model_cleanup_corruption(); }
		m.clear_value = value;
		CHECK(value == (m.exynos ? 1u : ~1u));
		if (!m.sticky_dbr) { m.r[0x58 / 4] = 0; }
		return;
	}
	if (off == 0x60 && !value && m.stop_stuck) { return; }
	if (off == 0x80 && !value && m.tm_stop_stuck) { return; }
	if (off == 0x60 && !value && m.stop_race) { m.r[0x58 / 4] = 2; }
	if (off == 0x60 && !value && m.stop_readiness_loss) { m.r[0x30 / 4] &= ~8u; }
	if (off == 0x60 && !value && m.stop_host_error) { m.r[0x20 / 4] |= 0x10000; }
	if (off == 0x60 && value && m.start_stuck) { return; }
	if (off == 0x50 || off == 0x54) {
		CHECK(m.r[0x60 / 4] == 0);
		m.base_writes++;
		if (m.base_stuck) { return; }
	}
	m.r[off / 4] = value;
	if (off == 0x80 && !value && !m.r[0x60 / 4]) { m.lists_stopped = true; }
	if (off == 0x58) { model_command(); }
}

static uint64_t model_smc(void)
{
	CHECK(m.exynos && !m.r[0x60 / 4] && !m.r[0x80 / 4]);
	CHECK(!m.r[0x58 / 4] && !m.r[0x78 / 4]);
	CHECK(!m.hce_pending && (m.r[0x34 / 4] & 1) && (m.r[0x30 / 4] & 0xff) == 0xf);
	if (m.allow_link_setup) { CHECK(m.link_live && !m.uic_pending && m.uic_gets == 3); }
	model_event('s', false, 0, 0);
	m.smcs++;
	return m.smc_result;
}
static void model_barrier(void) { model_event('b', false, 0, 0); m.barriers++; }
static void sweet_dma_sync_va(const void *va, uint32_t len) {
	CHECK(!m.exynos && (uintptr_t)va >= (uintptr_t)m.dma);
	CHECK((uintptr_t)va - (uintptr_t)m.dma <= m.size && len <= m.size - ((uintptr_t)va - (uintptr_t)m.dma));
	m.cache_ops++;
}
static void flush_dcache64(addr64_t pa, uint32_t len, int physical) {
	CHECK(!m.exynos && physical); (void)dma_at(pa, len); m.cache_ops++;
}
static void clean_dcache64(addr64_t pa, uint32_t len, int physical) {
	flush_dcache64(pa, len, physical);
}
static void nanoseconds_to_absolutetime(uint64_t n, uint64_t *out) { *out = n; }
static uint64_t mach_absolute_time(void) {
	model_event('t', false, 0, 0);
	if (!m.freeze_counter || !m.uic_commands) { m.now += 1000000; }
	return m.now;
}
static void delay(int us) {
	CHECK(us >= 0); model_event('d', false, 0, us);
	if (!m.freeze_counter || !m.uic_commands) { m.now += (uint64_t)us * 1000; }
}
static int assert_wait_deadline_with_leeway(event_t event, int mode, int urgency, uint64_t wake, uint64_t leeway) {
	m.wake = wake; return THREAD_WAITING;
}
static void assert_wait(event_t event, int mode) { m.waiting_hold = true; }
static void thread_block(void *continuation) {
	if (m.waiting_hold) { CHECK(m.expect_hold); longjmp(hold, 1); }
	m.sleeps++; m.now = m.wake;
}
static bool sweet_board_is(const char *name) { return m.exynos && !strcmp(name, "samsung,starlte"); }
static bool sweet_board_qualcomm(void) { return !m.exynos && !m.unknown_board; }
static int SecureDTLookupEntry(DTEntry start, const char *path, DTEntry *out) {
	if (!strcmp(path, "/sweet-ufs") && m.node_present) { *out = &m.ufs; return kSuccess; }
	if (!strcmp(path, "/chosen") && m.chosen_present) { *out = &m.chosen; return kSuccess; }
	return -1;
}
static int SecureDTGetProperty(DTEntry node, const char *name, const void **out, unsigned *size) {
	for (unsigned i = 0; i < node->count; i++) {
		if (!strcmp(node->props[i].name, name)) {
			*out = node->props[i].raw + 4; *size = node->props[i].size; return kSuccess;
		}
	}
	return -1;
}
static bool PE_parse_boot_argn(const char *name, void *out, unsigned size) {
	if (!strcmp(name, "ufsroot") && m.rootname[0]) { strlcpy(out, m.rootname, size); return true; }
	for (unsigned i = 0; i < m.nargs; i++) {
		if (!strcmp(name, m.args[i].name)) { CHECK(size == 4); memcpy(out, &m.args[i].value, 4); return true; }
	}
	return false;
}
static vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t size) {
	if (m.exynos && pa == 0x11110000 && size == 0x8000) {
		CHECK(m.allow_link_setup);
		if (m.fail_unipro_map) { return 0; }
		m.unipro_mapped = true; return (uintptr_t)&m.link_cnf;
	}
	if (m.exynos && pa == 0x11121100 && size == 0x200) {
		if (m.fail_vendor_map) { return 0; }
		m.vendor_mapped = true; return (uintptr_t)m.v;
	}
	CHECK(pa == (m.exynos ? 0x11120000u : 0x1d84000u) && size == (m.exynos ? 0x200u : 0x1000u));
	if (m.fail_hci_map) { return 0; }
	m.hci_mapped = true; return (uintptr_t)m.r;
}
static vm_offset_t ml_io_map_wcomb(vm_offset_t pa, vm_size_t size) {
	CHECK(m.exynos && pa == m.allocation_pa && size == 0x104000 && !m.pool_mapped);
	m.maps++;
	if (m.fail_pool_map) { return 0; }
	m.pool_mapped = true; m.pa = pa; m.size = size;
	return (uintptr_t)m.dma;
}
static kern_return_t kmem_alloc_contig(void *map, vm_offset_t *va, vm_size_t size, uint64_t mask,
    uint64_t max, uint64_t boundaries, int flags, int tag) {
	CHECK(!m.exynos && size <= MODEL_WINDOW && mask == 0x3fff && !m.pool_mapped);
	m.allocations++;
	if (m.fail_alloc || (m.fail_verify_alloc && size > 0x104000)) { return -1; }
	m.pool_mapped = true; m.pa = m.allocation_pa; m.size = size;
	memset(m.dma, 0, size); *va = (uintptr_t)m.dma; return KERN_SUCCESS;
}
static ppnum_t pmap_find_phys(void *pmap, addr64_t va) {
	CHECK(m.pool_mapped && va == (uintptr_t)m.dma); return m.pa >> 14;
}
static int bdevsw_add(int major, const void *sw) { m.bdevs++; return 17; }
static int cdevsw_add_with_bdev(int major, const void *sw, int bmajor) { m.cdevs++; CHECK(bmajor == 17); return 18; }
static void *devfs_make_node(dev_t dev, int type, int uid, int gid, int perms, const char *name) {
	m.nodes++; return (void *)1;
}

static void inject(enum fault fault, unsigned count, int opcode)
{
	m.fault = fault; m.fault_left = count; m.fault_op = opcode; m.fault_skip = 0;
}

static void transport(bool exynos, unsigned block)
{
	reset_model(exynos, block);
	su.exynos = exynos;
	su.block_size = block;
	su.device_blocks = m.disks[0].blocks;
	su.secsize = 512;
	su.dma_size = 0x104000;
	su.retries = 3;
	if (exynos) {
		CHECK(exynos_dma_handoff(&m.ufs) == 0);
		su.vendor = ml_io_map(0x11121100, 0x200);
	} else {
		CHECK(kmem_alloc_contig(NULL, &su.dma, su.dma_size, 0x3fff, 0, 0, 7, 0) == 0);
		su.dma_pa = m.pa;
	}
	su.regs = ml_io_map(exynos ? 0x11120000 : 0x1d84000, exynos ? 0x200 : 0x1000);
	CHECK(ufs_take_over_host() == 0 && su.host_owned);
}

static void crc_header(struct disk *d)
{
	put_le32(d->header + 16, 0);
	unsigned size = le32(d->header + 12);
	if (size <= sizeof(d->header)) { put_le32(d->header + 16, crc32(0, d->header, size)); }
}
static void crc_entries(struct disk *d)
{
	uint64_t size = (uint64_t)le32(d->header + 80) * le32(d->header + 84);
	CHECK(size <= sizeof(d->entries));
	put_le32(d->header + 88, crc32(0, d->entries, size)); crc_header(d);
}
static void label(struct disk *d, const char *name)
{
	CHECK(strlen(name) <= 36);
	memset(d->entries + 56, 0, 72);
	for (unsigned i = 0; name[i]; i++) { d->entries[56 + i * 2] = name[i]; }
	crc_entries(d);
}

static void test_transport(void)
{
	case_name = "Samsung descriptor and DMA boundaries";
	transport(true, 4096);
	uint8_t tur[6] = { 0 };
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == 0 && m.last_prds == 0);
	CHECK(ufs_read_capacity() == 0 && m.last_prds == 1 && m.last_len == 8);
	CHECK(ufs_read_blocks(0x100, 256) == 0 && m.last_prds == 4 && m.last_len == 0x100000);
	CHECK(!memcmp(m.dma + 0x4000, m.disks[0].data, 0x100000));
	CHECK(m.smcs == 1 && m.maps == 1 && m.allocations == 0 && m.cache_ops == 0 && m.barriers);
	CHECK((m.v[0] & 0x80000000u) && (m.v[0x40 / 4] & 0xa5000010u) == 0xa5000010u);
	CHECK(ufs_read_blocks(0x100, 65) == 0 && m.last_prds == 2);
	passed(case_name);

	case_name = "command bounds rejected before DMA reuse";
	unsigned commands = m.commands;
	CHECK(ufs_read_blocks(0, 0) == EINVAL);
	CHECK(ufs_read_blocks(0, 257) == EINVAL);
	CHECK(ufs_read_blocks(0x10000, 1) == EINVAL);
	CHECK(ufs_read_blocks(0xffff, 2) == EINVAL);
	CHECK(ufs_read_blocks(UINT64_MAX, 2) == EINVAL);
	CHECK(ufs_scsi_at(tur, 0, 0, false, 0x4000) == EINVAL);
	CHECK(ufs_scsi_at(tur, 17, 0, false, 0x4000) == EINVAL);
	CHECK(ufs_scsi_at(tur, 6, 4, false, 0x104000) == EINVAL);
	CHECK(ufs_scsi_at(tur, 6, 4, false, 0x4001) == EINVAL);
	CHECK(ufs_scsi_at(tur, 6, 7, false, 0x4000) == EINVAL);
	CHECK(ufs_scsi_at(tur, 6, 0x100004, false, 0x4000) == EINVAL);
	CHECK(ufs_scsi_at(tur, 6, 4, false, 0x3ffc) == EINVAL);
	CHECK(m.commands == commands);
	passed(case_name);

	case_name = "Samsung read-only opcode allowlist";
	uint8_t cdb[16] = { 0 };
	for (unsigned op = 0; op < 256; op++) {
		if (op == 0 || op == 0x25 || op == 0x28 || op == 0x88) { continue; }
		cdb[0] = op;
		CHECK(ufs_scsi(cdb, sizeof(cdb), 0, false) == EROFS);
	}
	CHECK(ufs_scsi(tur, sizeof(tur), 0, true) == EROFS);
	su.writable = true; su.first_block = 0x100; su.nblocks = 0x2000;
	CHECK(ufs_write_blocks(0x100, 1) == EROFS);
	CHECK(ufs_sync_cache() == EROFS);
	CHECK(m.commands == commands && !m.writes && !m.syncs);
	passed(case_name);
}

static void test_enable(void)
{
	case_name = "photographed HCE=0 / HCS=0x10f enable-only and read-only discovery";
	reset_model(true, 4096);
	m.r[0] = 0x1303ff0f;
	dev_t dev = 0;
	CHECK(sweet_ufs_rootdev(&dev) == 0 && su.host_owned && su.ready && dev);
	CHECK(m.hce_enables == 1 && !m.hce_pending && !m.hce_echo_reads && m.hce_init_reads >= 2);
	CHECK(m.hce_completed - m.hce_start == 6000000);
	CHECK(m.commands && m.smcs == 1 && !m.writes && !m.syncs);
	passed(case_name);

	case_name = "already-enabled Samsung host is not restarted";
	reset_model(true, 4096); m.r[0x34 / 4] = 1;
	CHECK(sweet_ufs_rootdev(&dev) == 0 && !m.hce_enables && m.commands && !m.writes);
	passed(case_name);

	case_name = "HCS upper power-change status does not replace readiness bits";
	for (unsigned power = 0; power < 3; power++) {
		reset_model(true, 4096);
		m.hce_hcs = m.r[0x30 / 4] = (power << 8) | 0xf;
		CHECK(sweet_ufs_rootdev(&dev) == 0 && m.hce_enables == 1);
	}
	passed(case_name);

	static const char *ready[] = { "device", "transfer", "task", "UIC" };
	for (unsigned enabled = 0; enabled < 2; enabled++) {
		for (unsigned bit = 0; bit < 4; bit++) {
			case_name = ready[bit];
			reset_model(true, 4096); m.r[0x34 / 4] = enabled;
			m.r[0x30 / 4] &= ~(1u << bit);
			CHECK(sweet_ufs_rootdev(&dev) == ENXIO && !su.ready && !su.host_owned);
			CHECK(!m.mmio_writes && !m.hce_enables && !m.smcs && !m.commands);
			passed(enabled ? "enabled host missing a readiness bit" : "HCE=0 missing a readiness bit, no enable");
		}
	}
	for (unsigned bit = 4; bit < 8; bit++) {
		case_name = "Samsung HCS low-byte error/reserved bit is not ready";
		reset_model(true, 4096); m.r[0x30 / 4] |= 1u << bit;
		CHECK(sweet_ufs_rootdev(&dev) == ENXIO && !m.mmio_writes && !m.hce_enables);
		passed(case_name);
	}
	static const unsigned error_bits[] = { 2, 7, 11, 12, 16, 17, 18 };
	for (unsigned i = 0; i < sizeof(error_bits) / sizeof(error_bits[0]); i++) {
		case_name = "inherited error IS is not cleared and ignored";
		reset_model(true, 4096); m.r[0x20 / 4] = 1u << error_bits[i];
		CHECK(sweet_ufs_rootdev(&dev) == EIO && !m.mmio_writes && !m.hce_enables && !m.smcs);
		CHECK(m.r[0x20 / 4] == 1u << error_bits[i]);
		passed(case_name);
	}
	case_name = "inherited UIC command-complete IS is not a host error";
	reset_model(true, 4096); m.r[0x20 / 4] = 1u << 10;
	CHECK(sweet_ufs_rootdev(&dev) == 0 && m.hce_enables == 1 && m.commands && !m.writes);
	passed(case_name);

	for (unsigned error = 0; error < 2; error++) {
		case_name = error ? "fatal IS during list stop prevents enable" : "readiness lost during list stop prevents enable";
		reset_model(true, 4096);
		m.stop_readiness_loss = !error; m.stop_host_error = error;
		CHECK(sweet_ufs_rootdev(&dev) == (error ? EIO : ENXIO));
		CHECK(!m.hce_enables && !m.smcs && !m.base_writes && !m.commands);
		passed(case_name);
	}
	for (unsigned bit = 0; bit < 4; bit++) {
		case_name = "enable loses device/list/UIC readiness, no cold link init";
		reset_model(true, 4096); m.hce_hcs &= ~(1u << bit);
		CHECK(sweet_ufs_rootdev(&dev) == ENXIO && su.dead && !su.host_owned && !su.ready);
		CHECK(m.hce_enables == 1 && !m.smcs && !m.base_writes && !m.commands);
		passed(case_name);
	}
	case_name = "fatal IS after enable prevents FMP or DMA";
	reset_model(true, 4096); m.hce_is = 0x10000;
	CHECK(sweet_ufs_rootdev(&dev) == EIO && su.dead && !m.smcs && !m.base_writes && !m.commands);
	passed(case_name);
	for (unsigned state = 0; state < 4; state++) {
		case_name = "request/list state changes after enable, no FMP or DMA";
		reset_model(true, 4096);
		switch (state) {
		case 0: m.hce_dbr = 2; break;
		case 1: m.hce_tm_dbr = 2; break;
		case 2: m.hce_rsr = 1; break;
		case 3: m.hce_tm_rsr = 1; break;
		}
		CHECK(sweet_ufs_rootdev(&dev) == EBUSY && su.dead && !su.host_owned && !su.ready);
		CHECK(m.hce_enables == 1 && !m.smcs && !m.base_writes && !m.commands);
		passed(case_name);
	}
	static const uint64_t latency[] = { 0, 1000000, 50000000, 51000000, 51000001 };
	for (unsigned i = 0; i < sizeof(latency) / sizeof(latency[0]); i++) {
		case_name = "HCE write-echo guard and bounded 1 ms + ten 5 ms wait";
		reset_model(true, 4096); m.hce_latency = latency[i];
		bool completes = i < 4;
		CHECK(sweet_ufs_rootdev(&dev) == (completes ? 0 : ETIMEDOUT));
		CHECK(m.hce_enables == 1 && !m.hce_echo_reads);
		CHECK(completes ? m.hce_completed - m.hce_start >= 1000000 &&
		    m.hce_completed - m.hce_start <= 51000000 : m.now - m.hce_start == 51000000);
		if (!completes) {
			CHECK(su.dead && !m.smcs && !m.base_writes && !m.commands);
			unsigned accesses = m.mmio_reads + m.mmio_writes;
			uint32_t dma_crc = crc32(0, m.dma, m.size);
			m.now += 1000000; model_enable_progress(); /* host completes too late */
			CHECK(!m.hce_pending && (m.r[0x34 / 4] & 1));
			CHECK(sweet_ufs_rootdev(&dev) == ETIMEDOUT && ufs_read_capacity() == EIO);
			CHECK(m.hce_enables == 1 && accesses == m.mmio_reads + m.mmio_writes);
			CHECK(crc32(0, m.dma, m.size) == dma_crc && !m.commands);
		}
		passed(case_name);
	}
	case_name = "Qualcomm HCE=0 still fails without an enable write";
	reset_model(false, 4096); m.r[0x34 / 4] = 0;
	CHECK(sweet_ufs_rootdev(&dev) == ENXIO && !m.mmio_writes && !m.hce_enables && !m.smcs && !m.commands);
	passed(case_name);
}

static void test_takeover(void)
{
	static const char *names[] = { "HCE enable timeout", "missing device", "host not ready",
	    "inherited transfer", "inherited task", "transfer list will not stop",
	    "task list will not stop", "request arrives during stop", "SMC rejected",
	    "PRDT setup rejected", "list base rejected", "list start rejected" };
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		case_name = names[i];
		reset_model(true, 4096);
		switch (i) {
		case 0: m.hce_stuck = true; break;
		case 1: m.r[0x30 / 4] &= ~1u; break;
		case 2: m.r[0x30 / 4] &= ~2u; break;
		case 3: m.r[0x58 / 4] = 4; break;
		case 4: m.r[0x78 / 4] = 2; break;
		case 5: m.stop_stuck = true; break;
		case 6: m.tm_stop_stuck = true; break;
		case 7: m.stop_race = true; break;
		case 8: m.smc_result = UINT64_MAX; break;
		case 9: m.prdt_stuck = true; m.v[0] = 7; break;
		case 10: m.base_stuck = true; break;
		case 11: m.start_stuck = true; break;
		}
		dev_t dev = 0;
		int error = sweet_ufs_rootdev(&dev);
		CHECK(error != 0 && !su.ready && !su.host_owned && !dev);
		CHECK(!m.commands && !m.writes && !m.bdevs);
		unsigned accesses = m.mmio_reads + m.mmio_writes;
		CHECK(sweet_ufs_rootdev(&dev) == error);
		CHECK(accesses == m.mmio_reads + m.mmio_writes && m.maps == 1);
		if (i < 8) { CHECK(!m.smcs && !m.base_writes); }
		if (i >= 1 && i < 8) { CHECK(!m.hce_enables); }
		if (i >= 1 && i <= 4) { CHECK(!m.mmio_writes); }
		if (i < 10) { CHECK(!m.base_writes); }
		passed(case_name);
	}
	case_name = "per-command Nexus refusal";
	transport(true, 4096);
	m.nexus_stuck = true;
	CHECK(ufs_read_blocks(0x100, 1) == EIO && su.dead && !m.commands);
	passed(case_name);
}

static void test_failures(void)
{
	for (unsigned sticky = 0; sticky < 2; sticky++) {
		case_name = sticky ? "timeout with stuck DBR / late DMA quarantine" : "timeout with cleared DBR / late DMA quarantine";
		reset_model(true, 4096);
		m.sticky_dbr = sticky;
		inject(TIMEOUT, 1, 0x25);
		dev_t dev = 0;
		CHECK(sweet_ufs_rootdev(&dev) == ETIMEDOUT && su.dead && !su.ready);
		CHECK(m.clear_value == 1 && ss.timeouts == 1 && ss.retries == 0);
		unsigned commands = m.commands, accesses = m.mmio_reads + m.mmio_writes;
		/* A controller can finish DMA after its request-clear acknowledgement. */
		memset(dma_at(m.pending_pa, m.pending_len), 0x39, m.pending_len);
		uint32_t digest = crc32(0, m.dma, m.size);
		CHECK(sweet_ufs_rootdev(&dev) == ETIMEDOUT);
		CHECK(ufs_read_capacity() == EIO);
		CHECK(ufs_read_blocks(1, 1) != 0);
		CHECK(crc32(0, m.dma, m.size) == digest);
		CHECK(m.commands == commands && m.maps == 1 && accesses == m.mmio_reads + m.mmio_writes);
		passed(case_name);
	}
	static const struct { enum fault fault; const char *name; bool dead; } errors[] = {
		{ BAD_OCS, "failed OCS", true }, { BAD_TYPE, "bad response transaction", true },
		{ BAD_LUN, "wrong response LUN", true }, { BAD_TAG, "wrong response task tag", true },
		{ BAD_RESPONSE, "UPIU response failure", true }, { HOST_ERROR, "fatal host interrupt", true },
		{ RESIDUAL, "nonzero residual", false }, { UNDERFLOW, "underflow flag", false },
		{ OVERFLOW, "overflow flag", false },
	};
	for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
		case_name = errors[i].name;
		transport(true, 4096);
		inject(errors[i].fault, 1, -1);
		CHECK(ufs_read_blocks(0x100, 1) == EIO);
		CHECK(m.commands == 1 && ss.retries == 0 && ss.cmd_failures == 1 && su.dead == errors[i].dead);
		passed(case_name);
	}
	for (unsigned i = 0; i < 3; i++) {
		case_name = i == 0 ? "UNIT ATTENTION retries" : i == 1 ? "BUSY retries" : "TASK SET FULL retries";
		transport(true, 4096);
		m.key = 6; m.asc = 0x29;
		inject(i == 0 ? SENSE : i == 1 ? BUSY : FULL, 1, -1);
		CHECK(ufs_read_blocks(0x100, 1) == 0 && !su.dead && m.commands == 2 && ss.retries == 1);
		passed(case_name);
	}
	case_name = "NOT READY retries are bounded";
	transport(true, 4096); m.key = 2; m.asc = 4;
	inject(SENSE, 20, -1);
	CHECK(ufs_read_blocks(0x100, 1) == EIO && m.commands == 4 && ss.retries == 3 && !su.dead);
	passed(case_name);
	case_name = "unsupported LUN is not a transport failure";
	transport(true, 4096); m.disks[0].present = false;
	CHECK(ufs_read_capacity() == EIO && m.commands == 1 && !su.dead && ss.last_sense == 0x52500);
	passed(case_name);
}

static unsigned occurrences(const char *needle)
{
	unsigned count = 0;
	for (const char *p = m.console; (p = strstr(p, needle)); p += strlen(needle)) { count++; }
	return count;
}

static void diagnostic_samples(unsigned count)
{
	CHECK(count <= 2 && occurrences("(live sample)") == count);
	CHECK(occurrences("STARLTE UFS: UIC PA ") == count);
	CHECK(occurrences("STARLTE UFS: UTRD0-3 LE ") == count);
	CHECK(occurrences("STARLTE UFS: UTRD4-7 LE ") == count);
	CHECK(occurrences("STARLTE UFS: RESP0-15 BE ") == count);
	for (unsigned i = 0; i < 5; i++) { CHECK(m.uic_reads[i] == count + m.pre_uic_samples); }
}

static void diagnostic_quarantine(void)
{
	CHECK(su.dead && m.commands == 1 && !m.writes && !ss.retries && ss.cmd_failures == 1);
	unsigned accesses = m.mmio_reads + m.mmio_writes;
	uint32_t digest = crc32(0, m.dma, m.size);
	uint8_t tur[6] = { 0 };
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == EIO);
	CHECK(accesses == m.mmio_reads + m.mmio_writes && m.commands == 1);
	CHECK(crc32(0, m.dma, m.size) == digest);
}

static void test_diagnostics(void)
{
	uint8_t tur[6] = { 0 };
	static const char *codes = "UIC PA 80000001 DL 80000002 NL 80000004 TL 80000040 DME 80000001";
	static const char *empty = "UIC PA 00000000 DL 00000000 NL 00000000 TL 00000000 DME 00000000";
	static const unsigned error_bits[] = { 2, 7, 11, 12, 16, 17, 18 };

	case_name = "healthy immediate completion has no diagnostic reads";
	transport(true, 4096);
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == 0);
	diagnostic_samples(0);
	passed(case_name);

	case_name = "healthy delayed completion has no diagnostic reads";
	transport(true, 4096); inject(PENDING_ERROR, 1, 0);
	m.fault_is = 0; m.complete_delay = 8000000;
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == 0 && !su.dead);
	diagnostic_samples(0);
	passed(case_name);

	for (unsigned sticky = 0; sticky < 2; sticky++) {
		case_name = "timeout snapshot precedes successful or stuck request clear";
		transport(true, 4096); inject(TIMEOUT, 1, 0);
		m.sticky_dbr = sticky; m.corrupt_on_clear = true;
		CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == ETIMEDOUT);
		diagnostic_samples(1);
		CHECK(!occurrences("first-error") && occurrences("timeout-preclear") == 1);
		CHECK(occurrences("DBR 1->1 RSR 1 TM 0/0") == 1);
		CHECK(occurrences("UTRD0-3 LE 11000000 00000000 0000000f 00000000 (OCS 0f)") == 1);
		CHECK(occurrences("UTRD4-7 LE 81101000 00000000 02000200 04000000") == 1);
		CHECK(occurrences("RESP0-15 BE 00000000 00000000 00000000 00000000") == 1);
		CHECK(occurrences(empty) == 1 && !ss.is_errors && ss.timeouts == 1);
		CHECK(m.clears == 1 && m.command_acks == 1 && m.clear_value == 1);
		CHECK(m.r[0x58 / 4] == sticky && le32(m.dma + 8) == 0x5a5a5a5a);
		diagnostic_quarantine();
		passed(case_name);
	}

	for (unsigned i = 0; i < sizeof(error_bits) / sizeof(error_bits[0]); i++) {
		case_name = "first pending error preserved across read-clear registers and timeout cleanup";
		transport(true, 4096); inject(PENDING_ERROR, 1, 0);
		m.fault_is = 1u << error_bits[i];
		m.uic_read_clear = m.drop_error_is = m.corrupt_on_clear = true;
		CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == ETIMEDOUT);
		diagnostic_samples(2);
		CHECK(occurrences("first-error") == 1 && occurrences("timeout-preclear") == 1);
		CHECK(occurrences(codes) == 1 && occurrences(empty) == 1);
		CHECK(occurrences("UTRD0-3 LE 11000000 00000000 1a00000f 00000000 (OCS 0f)") == 2);
		CHECK(occurrences("RESP0-15 BE 21400703 04050607 08090a0b 0c0d0e0f") == 2);
		CHECK(ss.last_is == m.fault_is && ss.is_errors == 1 && ss.timeouts == 1);
		CHECK(m.clears == 1 && m.command_acks == 1 && !m.r[0x20 / 4]);
		diagnostic_quarantine();
		passed(case_name);
	}

	for (unsigned i = 0; i < sizeof(error_bits) / sizeof(error_bits[0]); i++) {
		case_name = "completed error snapshot precedes interrupt acknowledgement and DMA mutation";
		transport(true, 4096); inject(COMPLETE_ERROR, 1, 0);
		m.fault_is = 1u << error_bits[i]; m.corrupt_on_ack = true;
		CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == EIO);
		diagnostic_samples(1);
		CHECK(!occurrences("first-error") && occurrences("complete-preack") == 1);
		CHECK(occurrences(codes) == 1 && occurrences("DBR 0->0 RSR 1 TM 0/0") == 1);
		CHECK(occurrences("UTRD0-3 LE 11000000 00000000 1a000000 00000000 (OCS 00)") == 1);
		CHECK(occurrences("RESP0-15 BE 21000000 00000000 00000000 00000000") == 1);
		CHECK(!ss.timeouts && ss.is_errors == 1 && !m.clears && m.command_acks == 1);
		diagnostic_quarantine();
		passed(case_name);
	}

	case_name = "earlier error cannot be hidden by an apparently good later completion";
	transport(true, 4096); inject(PENDING_ERROR, 1, 0);
	m.uic_read_clear = m.drop_error_is = true; m.complete_delay = 8000000;
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == EIO);
	diagnostic_samples(2);
	CHECK(occurrences("first-error") == 1 && occurrences("complete-preack") == 1);
	CHECK(occurrences("RESP0-15 BE 21400703 04050607 08090a0b 0c0d0e0f") == 1);
	CHECK(occurrences("RESP0-15 BE 21000000 00000000 00000000 00000000") == 1);
	CHECK(occurrences(codes) == 1 && occurrences(empty) == 1);
	CHECK(ss.last_is == 5 && !ss.timeouts && !m.clears);
	diagnostic_quarantine();
	passed(case_name);

	case_name = "error arriving at the deadline retained by terminal preclear sample";
	transport(true, 4096); inject(PENDING_ERROR, 1, 0);
	/* The model's first time read sets the deadline 1 ms after submission.
	 * Inject just after that deadline, not after the next full 1 ms tick.
	 */
	m.error_delay = 5001000001ull; m.drop_error_is = m.corrupt_on_clear = true;
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == ETIMEDOUT);
	diagnostic_samples(1);
	CHECK(!occurrences("first-error") && occurrences("timeout-preclear") == 1);
	CHECK(occurrences(codes) == 1 && ss.last_is == 4 && ss.is_errors == 1);
	diagnostic_quarantine();
	passed(case_name);

	case_name = "doorbell completion during a live sample is reported as a race";
	transport(true, 4096); inject(PENDING_ERROR, 1, 0);
	m.complete_on_uic = m.drop_error_is = true;
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == EIO);
	diagnostic_samples(2);
	CHECK(occurrences("DBR 1->0 RSR 1 TM 0/0") == 1);
	CHECK(occurrences("DBR 0->0 RSR 1 TM 0/0") == 1);
	CHECK(!ss.timeouts && !m.clears);
	diagnostic_quarantine();
	passed(case_name);

	case_name = "sleepable timeout retains bounded two-sample diagnostics and quarantine";
	transport(true, 4096); inject(PENDING_ERROR, 1, 0);
	su.wait = su.ready = true; m.sticky_dbr = true;
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == ETIMEDOUT && m.sleeps);
	diagnostic_samples(2);
	CHECK(m.now - m.command_start <= 5104000000ull && m.clears == 1);
	diagnostic_quarantine();
	passed(case_name);

	case_name = "raw layer values retain unknown and invalid bits without guessed recovery";
	transport(true, 4096); inject(COMPLETE_ERROR, 1, 0);
	static const uint32_t raw[] = { 0xdeadbeef, 0x76543210, 0, 0xffffffff, 0x12345678 };
	memcpy(m.fault_uic, raw, sizeof(raw));
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == EIO);
	diagnostic_samples(1);
	CHECK(occurrences("UIC PA deadbeef DL 76543210 NL 00000000 TL ffffffff DME 12345678") == 1);
	diagnostic_quarantine();
	passed(case_name);

	case_name = "nonerror pending interrupt is not classified as a first transport error";
	transport(true, 4096); inject(PENDING_ERROR, 1, 0); m.fault_is = 1u << 10;
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == ETIMEDOUT);
	diagnostic_samples(1);
	CHECK(!occurrences("first-error") && !ss.is_errors);
	diagnostic_quarantine();
	passed(case_name);

	case_name = "photographed TUR failure diagnostics hold before registration and quarantine DMA";
	reset_model(true, 4096); inject(PENDING_ERROR, 1, 0);
	arg("ufsprobe", 1); arg("ufsrw", 1); m.expect_hold = true;
	if (!setjmp(hold)) { dev_t dev; (void)sweet_ufs_rootdev(&dev); CHECK(false); }
	diagnostic_samples(2);
	CHECK(occurrences("(transport quarantined)") == 1 && occurrences("PROBE HOLD") == 1);
	CHECK(occurrences("writes 0, commands failed 1") == 1 && !su.writable && !su.ready);
	CHECK(!m.bdevs && !m.cdevs && !m.nodes && !services && !sweet_ufs_lock);
	diagnostic_quarantine();
	unsigned accesses = m.mmio_reads + m.mmio_writes;
	memset(dma_at(m.pending_pa, m.pending_len), 0x39, m.pending_len); /* late DMA after clear */
	uint32_t digest = crc32(0, m.dma, m.size);
	dev_t dev = 0;
	/* The test-owned longjmp exits the nonreturning hold before init_error
	 * can be assigned by the caller. The su.dead guard still prevents reuse.
	 */
	CHECK(sweet_ufs_rootdev(&dev) == EIO && !dev);
	CHECK(accesses == m.mmio_reads + m.mmio_writes && crc32(0, m.dma, m.size) == digest);
	passed(case_name);

	case_name = "first-command UIC timeout is cached when probe hold is disabled";
	reset_model(true, 4096); inject(PENDING_ERROR, 1, 0);
	CHECK(sweet_ufs_rootdev(&dev) == ETIMEDOUT && su.init_error == ETIMEDOUT);
	diagnostic_samples(2);
	accesses = m.mmio_reads + m.mmio_writes;
	digest = crc32(0, m.dma, m.size);
	CHECK(sweet_ufs_rootdev(&dev) == ETIMEDOUT && !dev);
	CHECK(accesses == m.mmio_reads + m.mmio_writes && crc32(0, m.dma, m.size) == digest);
	CHECK(!m.bdevs && !m.writes && !ss.retries && m.commands == 1);
	passed(case_name);
}

static void probe_to_hold(void)
{
	m.expect_hold = true;
	if (!setjmp(hold)) { dev_t dev; (void)sweet_ufs_rootdev(&dev); CHECK(false); }
	CHECK(!m.bdevs && !m.cdevs && !m.nodes && !services && !sweet_ufs_lock && !su.ready);
}

static void warm_probe(bool enabled)
{
	reset_model(true, 4096);
	m.startup_model = m.allow_link_setup = true;
	m.drop_dp_on_hce = m.reset_clocks_on_hce = true;
	m.uic_read_clear = true;
	m.r[0x34 / 4] = enabled;
	m.link_live = enabled;
	m.v[0xb4 / 4] = 0x5a5a07f0;
	m.v[0xb0 / 4] = 0xa5a5001f;
	m.v[0xfc / 4] = 0x22e;
	m.v[0x40 / 4] |= 1; /* NOP must actively clear the inherited SCSI Nexus. */
	arg("ufsprobe", 1); arg("ufslinkinit", 1); arg("ufsrw", 1);
}

static void warm_quarantine(void)
{
	CHECK(su.dead && !su.nop_ready && !su.ready && !su.writable);
	CHECK(!m.opcount[0] && !m.writes && !m.syncs && !ss.retries);
	CHECK(occurrences("PROBE HOLD") == 1 && !m.bdevs && !m.nodes);
	unsigned accesses = m.mmio_reads + m.mmio_writes, commands = m.commands;
	/* A late UIC completion or DMA cannot resurrect this boot's transport. */
	m.r[0x34 / 4] = 1; m.r[0x30 / 4] = 0x10f;
	m.r[0x58 / 4] = m.r[0x78 / 4] = 0;
	memset(m.dma + 0x1200, 0x39, 32);
	uint32_t digest = crc32(0, m.dma, m.size), value = 0;
	dev_t dev = 0;
	CHECK(sweet_ufs_rootdev(&dev) == EIO && !dev);
	CHECK(exynos_ufs_uic(1, 0x1561, &value) == EIO && exynos_ufs_nop() == EIO);
	CHECK(ufs_read_capacity() == EIO);
	CHECK(accesses == m.mmio_reads + m.mmio_writes && commands == m.commands);
	CHECK(crc32(0, m.dma, m.size) == digest);
}

static void preenable_probe(void)
{
	warm_probe(false);
	m.trace_boundaries = m.trace_through_nop = true;
	m.boundary_fault = 1;
	m.boundary_uic[0] = 0x80000010;
}

static void preenable_trace(unsigned samples, bool stopped)
{
	/* Independent trace: the original ten reads are unchanged. Only the
	 * exact residue adds twelve adjacent confirmation reads, before any
	 * clock/HCE write, barrier, delay or console operation.
	 */
	static const unsigned offsets[] = { 0x20, 0x34, 0x30, 0x58, 0x78, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x60, 0x80 };
	unsigned first = 0;
	while (first < m.pre_event_count && !(m.pre_events[first].kind == 'r' &&
	    !m.pre_events[first].vendor && m.pre_events[first].off == 0x38)) { first++; }
	CHECK(first >= 5 && first < m.pre_event_count && (samples == 1 || samples == 2));
	first -= 5;
	unsigned end = first;
	for (unsigned sample = 0; sample < samples; sample++) {
		unsigned count = sample ? 12 : 10;
		CHECK(end + count <= m.pre_event_count);
		for (unsigned i = 0; i < count; i++) {
			const typeof(m.pre_events[0]) *e = &m.pre_events[end + i];
			CHECK(e->kind == 'r' && !e->vendor && e->off == offsets[i]);
		}
		end += count;
	}
	if (stopped) {
		for (unsigned i = end; i < m.pre_event_count; i++) { CHECK(m.pre_events[i].kind == 'p'); }
		CHECK(m.mmio_writes == m.boundary_last_writes);
	} else {
		CHECK(samples == 2 && end < m.pre_event_count);
		const typeof(m.pre_events[0]) *e = &m.pre_events[end];
		CHECK(e->kind == 'r' && e->vendor && e->off == 0xb4); /* First clock RMW. */
	}
}

static void preenable_failed(unsigned samples, int error)
{
	probe_to_hold();
	preenable_trace(samples, true);
	CHECK(m.boundary_fault_sent && su.boundary_seen == (samples == 2 ? 9u : 1u));
	CHECK(!m.hce_enables && !m.clock_writes && !m.uic_commands && !m.commands && !m.smcs && !m.base_writes);
	CHECK(!ss.cmd_failures && !ss.timeouts && !ss.retries && !m.writes && !m.syncs && !m.clears);
	for (unsigned i = 0; i < 5; i++) { CHECK(su.boundary[0].uic[i] == m.boundary_uic[i]); }
	const struct exynos_ufs_boundary *s = &su.boundary[samples == 2 ? 3 : 0];
	CHECK(s->error == error && ss.last_is == s->is && ss.last_hcs == s->hcs);
	CHECK(ss.is_errors == !!(s->is & MODEL_IS_ERRORS));
	CHECK(occurrences("STARLTE UFS: boundary ") == 2 * samples && occurrences(" STOP\n") == 1);
	CHECK(occurrences("boundary pre-HCE-clear ") == (samples == 2));
	char line[128];
	snprintf(line, sizeof(line), "probe FAILED (%d), writes 0, commands failed 0", error);
	CHECK(occurrences(line) == 1);
	unsigned accesses = m.mmio_reads + m.mmio_writes;
	warm_quarantine();
	size_t console_size = m.console_size;
	exynos_ufs_log_boundaries();
	CHECK(accesses == m.mmio_reads + m.mmio_writes && console_size == m.console_size);
}

static void test_preenable_linereset(void)
{
	for (unsigned state = 0; state < 3; state++) {
		case_name = "isolated pre-HCE LINERESET clears before startup/NOP and synthetic media reads";
		preenable_probe();
		m.drop_dp_on_hce = state != 1;
		if (state == 2) { m.uic_fault_at = 5; m.uic_fault_is = 4; m.uic_fault_codes[0] = 0x80000010; }
		probe_to_hold();
		preenable_trace(2, false);
		CHECK(!su.dead && su.nop_ready && su.host_owned && su.boundary_seen == 15);
		CHECK(su.boundary[0].uic[0] == 0x80000010 && !su.boundary[0].error);
		const struct exynos_ufs_boundary *s = &su.boundary[3];
		CHECK(!s->is && !s->hce && s->hcs == 0x10f && !s->dbr && !s->tm_dbr && !s->rsr && !s->tm_rsr && !s->error);
		for (unsigned i = 0; i < 5; i++) { CHECK(!s->uic[i]); }
		CHECK(m.hce_enables == 1 && m.clock_writes == 6 && m.link_starts == 1 && m.uic_commands == 8);
		CHECK(m.nops == 1 && m.nop_replies == 1 && m.opcount[0] == 1 && ss.reads >= 4);
		CHECK(su.device_blocks == 0x10000 && su.first_block == 0x100 && su.nblocks == 0x2000);
		CHECK(!m.writes && !m.syncs && !ss.cmd_failures && !ss.is_errors && !ss.retries);
		CHECK(occurrences("STARLTE UFS: boundary ") == 8 && occurrences("boundary pre-HCE-clear ") == 1);
		CHECK(occurrences("probe PASSED (0), writes 0, commands failed 0") == 1);
		size_t console_size = m.console_size;
		unsigned accesses = m.mmio_reads + m.mmio_writes;
		exynos_ufs_log_boundaries();
		CHECK(console_size == m.console_size && accesses == m.mmio_reads + m.mmio_writes);
		passed(case_name);
	}

	case_name = "persistent pre-HCE PA LINERESET stops after one clearing check, without HCE";
	preenable_probe(); m.uic_read_clear = false;
	preenable_failed(2, EIO);
	CHECK(su.boundary[3].uic[0] == 0x80000010 && m.uic_reads[0] == 2);
	passed(case_name);
	case_name = "recurrent read-cleared PA LINERESET remains fatal in the clearing confirmation";
	preenable_probe(); m.clear_fault = true; m.clear_uic[0] = 0x80000010;
	preenable_failed(2, EIO);
	CHECK(m.clear_fault_sent && !m.r[0x38 / 4] && su.boundary[3].uic[0] == 0x80000010);
	passed(case_name);

	for (unsigned bit = 0; bit < 32; bit++) {
		case_name = "every bit-neighbor of exact pre-HCE PA LINERESET is rejected, despite read-clearing";
		preenable_probe(); m.boundary_uic[0] ^= 1u << bit;
		preenable_failed(1, EIO); passed(case_name);
		case_name = "any nonzero original IS excludes the pre-HCE allowance, including nonerror bits";
		preenable_probe(); m.boundary_is = 1u << bit; m.boundary_is_read_clear = true;
		preenable_failed(1, EIO); passed(case_name);
	}
	static const uint32_t raw_codes[] = { 1, 0x80000000, 0xdeadbeef };
	for (unsigned layer = 1; layer < 5; layer++) {
		for (unsigned raw = 0; raw < 3; raw++) {
			case_name = "any other original layer indication excludes pre-HCE LINERESET consumption";
			preenable_probe(); m.boundary_uic[layer] = raw_codes[raw];
			preenable_failed(1, EIO); passed(case_name);
		}
	}
	static const uint32_t hce_values[] = { 1, 2, 0x80000000 };
	for (unsigned i = 0; i < 3; i++) {
		case_name = "enabled/unknown nonzero HCE never consumes pre-enable LINERESET or restarts";
		preenable_probe(); m.r[0x34 / 4] = hce_values[i];
		preenable_failed(1, EIO); passed(case_name);
	}
	for (unsigned option = 0; option < 2; option++) {
		case_name = "absent/zero ufslinkinit keeps the original strict pre-enable boundary";
		preenable_probe(); m.nargs = 0; arg("ufsprobe", 1);
		if (option) { arg("ufslinkinit", 0); }
		m.allow_link_setup = false;
		preenable_failed(1, EIO); passed(case_name);
	}
	for (unsigned bit = 0; bit < 4; bit++) {
		case_name = "original readiness loss excludes isolated pre-HCE residue handling";
		preenable_probe(); m.boundary_hcs_clear = 1u << bit;
		preenable_failed(1, EIO); passed(case_name);
	}
	for (unsigned task = 0; task < 2; task++) {
		case_name = "original pending transfer/task excludes isolated pre-HCE residue handling";
		preenable_probe();
		if (task) { m.boundary_tm_dbr = 1; } else { m.boundary_dbr = 1; }
		preenable_failed(1, EIO); passed(case_name);
	}

	for (unsigned layer = 0; layer < 5; layer++) {
		for (unsigned raw = 0; raw < 3; raw++) {
			case_name = "new layer fault in clearing confirmation is retained and fatal even when it read-clears";
			preenable_probe(); m.clear_fault = true; m.clear_uic[layer] = raw_codes[raw];
			preenable_failed(2, EIO);
			CHECK(m.clear_fault_sent && su.boundary[3].uic[layer] == raw_codes[raw] && !m.r[0x38 / 4 + layer]);
			passed(case_name);
		}
	}
	for (unsigned bit = 0; bit < 32; bit++) {
		case_name = "any fresh IS in pre-HCE confirmation is retained and rejected despite read-clearing";
		preenable_probe(); m.clear_fault = m.clear_is_read_clear = true; m.clear_is = 1u << bit;
		preenable_failed(2, EIO);
		CHECK(m.clear_fault_sent && su.boundary[3].is == m.clear_is && !m.r[0x20 / 4]);
		passed(case_name);
	}
	for (unsigned i = 0; i < 3; i++) {
		case_name = "HCE changing during clearing confirmation prevents enabling stale HCE state";
		preenable_probe(); m.clear_fault = true; m.clear_hce = hce_values[i];
		preenable_failed(2, ENXIO); passed(case_name);
	}
	for (unsigned bit = 0; bit < 8; bit++) {
		case_name = "fresh low-byte HCS readiness/reserved error changes prevent HCE setup";
		preenable_probe(); m.clear_fault = true;
		if (bit < 4) { m.clear_hcs_clear = 1u << bit; } else { m.clear_hcs_set = 1u << bit; }
		preenable_failed(2, ENXIO); passed(case_name);
	}
	for (unsigned list = 0; list < 4; list++) {
		for (unsigned raw = 0; raw < 3; raw++) {
			case_name = "fresh pending/restarted/unknown transfer or task list state prevents HCE setup";
			preenable_probe(); m.clear_fault = true;
			uint32_t *fields[] = { &m.clear_dbr, &m.clear_tm_dbr, &m.clear_rsr, &m.clear_tm_rsr };
			*fields[list] = raw_codes[raw];
			preenable_failed(2, EBUSY); passed(case_name);
		}
	}
	for (unsigned layer = 0; layer < 5; layer++) {
		case_name = "post-HCE layer error cannot use the earlier clearing allowance at first UIC SET";
		preenable_probe(); m.hce_uic[layer] = layer ? 0x80000001 : 0x80000010;
		probe_to_hold(); preenable_trace(2, false);
		CHECK(m.hce_enables == 1 && !m.uic_commands && !m.nops && !m.smcs && !ss.cmd_failures);
		CHECK(su.boundary_seen == 9 && !su.boundary[3].error && occurrences("sent 0 done 0") == 1);
		warm_quarantine(); passed(case_name);
	}
	case_name = "PA LINERESET during first UIC SET is not inherited pre-HCE residue";
	preenable_probe(); m.uic_fault_at = 1; m.uic_fault_codes[0] = 0x80000010;
	probe_to_hold();
	CHECK(m.uic_commands == 1 && !m.smcs && !m.nops && !ss.cmd_failures);
	warm_quarantine(); passed(case_name);
	for (unsigned boundary = 2; boundary <= 3; boundary++) {
		for (unsigned is = 0; is < 2; is++) {
			case_name = "recurring PA LINERESET remains fatal at pre-NOP/pre-TUR, even with IS=0";
			preenable_probe(); m.late_boundary = boundary;
			m.late_boundary_is = is ? 4 : 0; m.late_boundary_uic[0] = 0x80000010;
			probe_to_hold();
			CHECK(su.dead && m.late_boundary_sent && !m.opcount[0] && m.nops == (boundary == 2 ? 1u : 0u));
			CHECK(!ss.cmd_failures && !ss.timeouts && !m.writes && !ss.retries && !m.clears);
			CHECK(su.boundary[boundary == 2 ? 1 : 2].error == EIO && !su.boundary[3].error);
			unsigned accesses = m.mmio_reads + m.mmio_writes, commands = m.commands;
			uint32_t digest = crc32(0, m.dma, m.size), value = 0;
			dev_t dev = 0;
			CHECK(sweet_ufs_rootdev(&dev) == EIO && !dev && exynos_ufs_uic(1, 0x1561, &value) == EIO);
			CHECK(exynos_ufs_nop() == EIO && ufs_read_capacity() == EIO);
			CHECK(accesses == m.mmio_reads + m.mmio_writes && commands == m.commands && crc32(0, m.dma, m.size) == digest);
			passed(case_name);
		}
	}
}

static void test_link_startup(void)
{
	for (unsigned state = 0; state < 3; state++) {
		bool enabled = state == 2;
		case_name = enabled ? "inherited enabled link receives GET/NOP checks without LINKSTARTUP" :
		    state ? "HCE-invalidated transport requires startup even when HCS still looks ready" :
		    "HCE drops Device Present; local CPort/startup/NOP restore readable media";
		warm_probe(enabled);
		m.drop_dp_on_hce = state == 0;
		m.trace_boundaries = m.trace_through_nop = true;
		probe_to_hold();
		CHECK(!su.dead && su.nop_ready && su.host_owned && su.boundary_seen == 7);
		CHECK(m.hce_enables == !enabled && m.link_starts == !enabled && m.unipro_reads == !enabled);
		CHECK(m.uic_commands == (enabled ? 3u : 8u) && m.uic_gets == 3 && m.uic_sets == (enabled ? 0u : 4u));
		CHECK(m.nops == 1 && m.nop_replies == 1 && m.opcount[0] == 1 && m.commands > 1);
		CHECK(su.device_blocks == 0x10000 && su.block_size == 4096 && su.first_block == 0x100 && su.nblocks == 0x2000);
		CHECK(ss.reads >= 4 && !ss.cmd_failures && !m.writes && !m.syncs && !m.cache_ops);
		CHECK(m.clock_writes == (enabled ? 3u : 6u) && model_clocks_running());
		CHECK(m.v[0xb4 / 4] == 0x5a5a07e0 && m.v[0xb0 / 4] == 0xa5a50008 && m.v[0xfc / 4] == 0x22f);
		CHECK(occurrences("boundary pre-NOP ") == 1 && occurrences("probe PASSED (0)") == 1);
		static const unsigned attrs[] = { 0x3000, 0x3001, 0x4021, 0x4020, 0, 0x1561, 0x1581, 0x4020 };
		for (unsigned i = 0; i < m.uic_commands; i++) {
			unsigned expected = enabled ? i + 5 : i;
			const typeof(m.uic_history[0]) *u = &m.uic_history[i];
			CHECK(u->arg1 == attrs[expected] << 16 && !u->arg2);
			CHECK(u->command == (expected < 4 ? 2u : expected == 4 ? 0x16u : 1u));
			CHECK(u->arg3 == (expected > 0 && expected < 4 ? 1u : 0u));
		}
		/* The first SCSI doorbell still follows the original ten-read TUR
		 * boundary immediately, even though NOP used this slot first.
		 */
		static const unsigned offsets[] = { 0x20, 0x34, 0x30, 0x58, 0x78, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x58 };
		CHECK(m.pre_event_count > 11);
		for (unsigned i = 0; i < 11; i++) {
			const typeof(m.pre_events[0]) *e = &m.pre_events[m.pre_event_count - 11 + i];
			CHECK(!e->vendor && e->off == offsets[i] && e->kind == (i == 10 ? 'w' : 'r'));
		}
		passed(case_name);
	}

	case_name = "enable-only comparison times out on HCS-ready but invalidated transport";
	warm_probe(false); m.allow_link_setup = false; m.drop_dp_on_hce = false;
	m.nargs = 0; arg("ufsprobe", 1); arg("ufslinkinit", 0);
	probe_to_hold();
	CHECK(su.dead && !m.uic_commands && !m.nops && m.opcount[0] == 1 && ss.timeouts == 1);
	CHECK(!m.clock_writes && !m.unipro_mapped);
	passed(case_name);

	for (unsigned probe = 0; probe < 3; probe++) {
		case_name = "startup requires exact probe opt-in before any MMIO";
		warm_probe(false); m.nargs = 0; arg("ufslinkinit", 1);
		if (probe) { arg("ufsprobe", probe == 1 ? 0 : 2); }
		dev_t dev = 0;
		CHECK(sweet_ufs_rootdev(&dev) == EINVAL && !m.mmio_reads && !m.mmio_writes && !m.maps);
		passed(case_name);
	}
	case_name = "non-one startup option is rejected in the read-only hold";
	warm_probe(false); m.args[1].value = 2;
	probe_to_hold();
	CHECK(su.init_error == EINVAL && !m.mmio_reads && !m.mmio_writes);
	passed(case_name);

	for (unsigned fault = 0; fault < 4; fault++) {
		case_name = "UniPro window must be exact and mapped before MMIO";
		warm_probe(false);
		if (fault == 0) { property(&m.ufs, "unipro-reg")->size = 0; }
		if (fault == 1) { set_pair("unipro-reg", 0x11120000, 0x8000); }
		if (fault == 2) { set_pair("unipro-reg", 0x11110000, 0x7854); }
		if (fault == 3) { m.fail_unipro_map = true; }
		probe_to_hold();
		CHECK(su.init_error == (fault == 3 ? ENOMEM : ENXIO) && !m.mmio_reads && !m.mmio_writes);
		passed(case_name);
	}
	case_name = "Qualcomm ignores the Samsung startup option";
	reset_model(false, 4096); label(&m.disks[0], "userdata");
	arg("ufsprobe", 1); arg("ufslinkinit", 1); arg("ufsrw", 1);
	dev_t dev = 0;
	CHECK(sweet_ufs_rootdev(&dev) == 0 && dev && su.writable && !m.uic_commands && !m.nops && !m.unipro_mapped);
	passed(case_name);
}

static void test_uic_failures(void)
{
	static const unsigned clocks[] = { 0xb4, 0xb0, 0xfc };
	for (unsigned i = 0; i < 3; i++) {
		case_name = "rejected clock control stops before HCE/UIC or DMA";
		warm_probe(false); m.clock_stuck = clocks[i];
		probe_to_hold();
		CHECK(!m.hce_enables && !m.uic_commands && !m.commands && !m.smcs);
		warm_quarantine(); passed(case_name);
	}
	case_name = "stale UIC completion must clear before the first command";
	warm_probe(false); m.hce_is = 0x400; m.stale_completion_stuck = true;
	probe_to_hold();
	CHECK(!m.uic_commands && !m.smcs && !m.commands && occurrences("sent 0 done 0") == 1);
	warm_quarantine(); passed(case_name);
	case_name = "cleared stale completion does not replace the new UIC transaction";
	warm_probe(false); m.hce_is = 0x400;
	probe_to_hold();
	CHECK(!su.dead && su.nop_ready && m.uic_commands == 8 && m.uic_polls >= 8);
	passed(case_name);

	for (unsigned command = 1; command <= 8; command++) {
		case_name = "every UIC SET/LINKSTARTUP/GET timeout stops the sequence without retry";
		warm_probe(false); m.uic_fault_at = command; m.uic_stuck = true;
		probe_to_hold();
		CHECK(m.uic_commands == command && !m.smcs && !m.commands && !ss.cmd_failures);
		CHECK(m.now - m.uic_start >= 500000000 && m.now - m.uic_start < 503000000);
		CHECK(occurrences("done 0") == 1);
		warm_quarantine(); passed(case_name);
	}
	case_name = "UIC timeout has a finite poll budget even with a stopped counter";
	warm_probe(false); m.uic_fault_at = 1; m.uic_stuck = m.freeze_counter = true;
	probe_to_hold();
	CHECK(m.uic_commands == 1 && m.uic_polls == 5000 && m.now == m.uic_start);
	warm_quarantine(); passed(case_name);

	for (unsigned command = 1; command <= 8; command++) {
		if (command == 5) { continue; }
		case_name = "nonzero DME attribute result prevents NOP/SCSI";
		warm_probe(false); m.uic_fault_at = command; m.uic_result = 1;
		probe_to_hold();
		CHECK(m.uic_commands == command && !m.smcs && !m.commands);
		warm_quarantine(); passed(case_name);
	}
	case_name = "LINKSTARTUP uses successful UniPro confirmation despite generic ARG2 failure";
	warm_probe(false); m.link_arg2 = 7; m.link_result = 0x1200;
	probe_to_hold();
	CHECK(!su.dead && su.nop_ready && m.unipro_reads == 1 && occurrences("result 0 arg2 7") == 1);
	passed(case_name);
	case_name = "failed UniPro confirmation cannot be hidden by successful generic ARG2";
	warm_probe(false); m.link_result = 7;
	probe_to_hold();
	CHECK(m.uic_commands == 5 && m.unipro_reads == 1 && !m.smcs && !m.commands);
	warm_quarantine(); passed(case_name);

	for (unsigned fault = 0; fault < 8; fault++) {
		case_name = "UIC completion with readiness/list/acknowledgement failure quarantines";
		warm_probe(false); m.uic_fault_at = 8;
		if (fault == 0) { m.uic_hcs_clear = 8; }
		if (fault == 1) { m.uic_drop_hce = true; }
		if (fault >= 2 && fault <= 5) {
			static const unsigned regs[] = { 0x58, 0x78, 0x60, 0x80 };
			m.uic_busy_reg = regs[fault - 2];
		}
		if (fault == 6) { m.uic_ack_stuck = true; }
		if (fault == 7) { m.uic_ack_error = 0x10000; }
		probe_to_hold();
		CHECK(m.uic_commands == 8 && !m.smcs && !m.commands);
		warm_quarantine(); passed(case_name);
	}
	for (unsigned lane = 0; lane < 5; lane++) {
		case_name = "invalid connected lanes/CPort prevent request-list setup";
		warm_probe(false);
		if (lane == 0) { m.tx_lanes = 0; }
		if (lane == 1) { m.tx_lanes = 3; }
		if (lane == 2) { m.rx_lanes = 0; }
		if (lane == 3) { m.rx_lanes = 3; }
		if (lane == 4) { m.connected = 0; }
		probe_to_hold();
		CHECK(m.uic_gets == 3 && !m.smcs && !m.base_writes && !m.nops);
		warm_quarantine(); passed(case_name);
	}

	case_name = "only completed successful LINKSTARTUP consumes its exact PA LINERESET";
	warm_probe(false); m.uic_fault_at = 5; m.uic_fault_is = 4; m.uic_fault_codes[0] = 0x80000010;
	m.uic_error_read_clear = true; /* completion must not hide the earlier IS */
	probe_to_hold();
	CHECK(!su.dead && su.nop_ready && occurrences("UIC setup IS 00000504") == 1);
	CHECK(!m.r[0x38 / 4] && !m.r[0x20 / 4] && !ss.is_errors);
	passed(case_name);
	for (unsigned fault = 0; fault < 10; fault++) {
		case_name = "LINERESET allowance excludes failed/incomplete/non-startup and other errors";
		warm_probe(false); m.uic_fault_at = 5; m.uic_fault_is = 4; m.uic_fault_codes[0] = 0x80000010;
		if (fault == 0) { m.link_result = 1; }
		if (fault == 1) { m.uic_stuck = true; }
		if (fault == 2) { m.uic_fault_at = 4; }
		if (fault == 3) { m.uic_fault_at = 6; }
		if (fault == 4) { m.uic_fault_codes[0] |= 1; }
		if (fault >= 5 && fault <= 8) { m.uic_fault_codes[fault - 4] = 0x80000001; }
		if (fault == 9) { m.uic_fault_is |= 0x10000; }
		probe_to_hold();
		CHECK(m.uic_commands == m.uic_fault_at && !m.smcs && !m.nops);
		warm_quarantine(); passed(case_name);
	}
	for (unsigned layer = 0; layer < 5; layer++) {
		case_name = "nonzero UIC layer code is rejected even without IS.UIC_ERROR";
		warm_probe(false); m.uic_fault_at = 8; m.uic_fault_codes[layer] = 1;
		probe_to_hold();
		CHECK(!m.smcs && !m.nops);
		warm_quarantine(); passed(case_name);
	}
}

static void test_nop(void)
{
	case_name = "SCSI is refused before NOP without touching descriptors or MMIO";
	transport(true, 4096); su.link_init = true;
	unsigned accesses = m.mmio_reads + m.mmio_writes;
	uint32_t digest = crc32(0, m.dma, m.size);
	CHECK(ufs_read_capacity() == EIO && !m.commands && !su.nop_ready);
	CHECK(accesses == m.mmio_reads + m.mmio_writes && crc32(0, m.dma, m.size) == digest);
	passed(case_name);

	for (unsigned boundary = 1; boundary <= 3; boundary++) {
		case_name = "PA LINERESET at pre-enable/pre-NOP/pre-TUR still stops before the next doorbell";
		warm_probe(false); m.boundary_fault = boundary;
		m.boundary_is = 4; m.boundary_uic[0] = 0x80000010;
		m.boundary_is_read_clear = true;
		probe_to_hold();
		CHECK(su.dead && !m.opcount[0] && m.nops == (boundary == 2 ? 1u : 0u));
		CHECK(!ss.cmd_failures && !ss.timeouts && !m.writes && !ss.retries);
		if (boundary == 2) { CHECK(su.nop_ready); } else { warm_quarantine(); }
		passed(case_name);
	}
	for (unsigned fault = 0; fault < 4; fault++) {
		case_name = "pre-NOP host/device/task readiness is checked before submission";
		warm_probe(false); m.boundary_fault = 3;
		if (fault == 0) { m.boundary_hce_off = true; }
		if (fault == 1) { m.boundary_hcs_clear = 1; }
		if (fault == 2) { m.boundary_tm_dbr = 1; }
		if (fault == 3) { m.boundary_dbr = 1; }
		probe_to_hold();
		CHECK(!m.commands && !ss.cmd_failures);
		warm_quarantine(); passed(case_name);
	}

	static const enum fault nop_faults[] = { BAD_OCS, BAD_TYPE, BAD_TAG, BAD_LUN, BAD_RESPONSE, MISSING_RESPONSE, HOST_ERROR };
	for (unsigned i = 0; i < sizeof(nop_faults) / sizeof(nop_faults[0]); i++) {
		case_name = "NOP IN requires OCS, transaction, task tag and a real valid response";
		warm_probe(false); inject(nop_faults[i], 1, 256);
		probe_to_hold();
		CHECK(m.nops == 1 && m.commands == 1 && ss.cmd_failures == 1);
		warm_quarantine(); passed(case_name);
	}
	static const unsigned nop_fields[] = { 1, 4, 5, 7, 8, 10, 11, 12, 31 };
	for (unsigned i = 0; i < sizeof(nop_fields) / sizeof(nop_fields[0]); i++) {
		case_name = "NOP IN flags/EHS/data length/reserved fields cannot masquerade as a handshake";
		warm_probe(false); m.nop_corrupt_byte = nop_fields[i];
		probe_to_hold();
		CHECK(m.nops == 1 && m.commands == 1 && ss.cmd_failures == 1);
		warm_quarantine(); passed(case_name);
	}
	case_name = "NOP requires clearing the SCSI Nexus bit";
	warm_probe(false); m.nexus_stuck = true;
	probe_to_hold();
	CHECK(!m.commands && !ss.cmd_failures);
	warm_quarantine(); passed(case_name);
	for (unsigned fault = 0; fault < 3; fault++) {
		case_name = "NOP timeout/late completion uses permanent quarantine and bounded positive-bit cleanup";
		warm_probe(false); inject(fault == 2 ? PENDING_ERROR : TIMEOUT, 1, 256);
		m.sticky_dbr = fault == 1;
		if (fault == 2) {
			m.fault_is = 4; memset(m.fault_uic, 0, sizeof(m.fault_uic)); m.fault_uic[0] = 0x80000010;
			m.drop_error_is = true; m.complete_delay = 8000000;
		}
		probe_to_hold();
		CHECK(m.nops == 1 && m.commands == 1 && ss.cmd_failures == 1);
		CHECK(ss.timeouts == (fault < 2) && m.clears == (fault < 2));
		if (fault < 2) { CHECK(m.clear_value == 1 && m.now - m.command_start < 5110000000ull); }
		else { CHECK(m.nop_replies == 1 && ss.is_errors == 1); }
		warm_quarantine(); passed(case_name);
	}
}

static void boundary_probe(void)
{
	reset_model(true, 4096);
	arg("ufsprobe", 1); arg("ufsrw", 1);
	m.trace_boundaries = true;
	m.uic_read_clear = m.boundary_is_read_clear = true;
}

static void boundary_samples(unsigned count)
{
	CHECK(count <= 2 && m.pre_uic_samples == count);
	CHECK(occurrences("STARLTE UFS: boundary ") == 2 * count);
	CHECK(occurrences("boundary pre-enable ") == (count > 0));
	CHECK(occurrences("boundary pre-TUR ") == (count > 1));
	CHECK(occurrences("boundary UIC PA ") == count);
}

static void boundary_trace(unsigned count, bool submitted)
{
	/* Ten standard-window reads per sample, in source order, without
	 * intervening MMIO writes, SMC, delay, barrier or console operations.
	 */
	static const uint32_t offsets[] = { 0x20, 0x34, 0x30, 0x58, 0x78, 0x38, 0x3c, 0x40, 0x44, 0x48 };
	unsigned starts[2], found = 0;
	for (unsigned i = 0; i < m.pre_event_count; i++) {
		const typeof(m.pre_events[0]) *e = &m.pre_events[i];
		if (e->kind != 'r' || e->vendor || e->off != 0x38) { continue; }
		CHECK(found < 2 && i >= 5 && i + 4 < m.pre_event_count);
		unsigned start = i - 5;
		starts[found++] = start;
		for (unsigned j = 0; j < 10; j++) {
			e = &m.pre_events[start + j];
			CHECK(e->kind == 'r' && !e->vendor && e->off == offsets[j]);
		}
	}
	CHECK(found == count && count > 0);
	int ack = -1, transfer_stop = -1, task_stop = -1;
	bool transfer_verified = false, task_verified = false;
	for (unsigned i = 0; i < starts[0]; i++) {
		const typeof(m.pre_events[0]) *e = &m.pre_events[i];
		CHECK(e->kind != 's'); /* First sample precedes FMP and host setup. */
		if (e->kind == 'w') {
			CHECK(!e->vendor && (e->off == 0x24 || e->off == 0x20 || e->off == 0x60 || e->off == 0x80));
			if (e->off == 0x20) { ack = i; }
			if (e->off == 0x60 && !e->value) { transfer_stop = i; }
			if (e->off == 0x80 && !e->value) { task_stop = i; }
		}
		if (e->kind == 'r' && !e->vendor && e->off == 0x60 && transfer_stop >= 0 && !e->value) {
			transfer_verified = true;
		}
		if (e->kind == 'r' && !e->vendor && e->off == 0x80 && task_stop >= 0 && !e->value) {
			task_verified = true;
		}
	}
	CHECK(ack >= 0 && transfer_stop > ack && task_stop > transfer_stop && transfer_verified && task_verified);
	if (count == 2) {
		CHECK(starts[1] > starts[0] + 10);
		const typeof(m.pre_events[0]) *e = &m.pre_events[starts[1] - 1];
		CHECK(e->kind == 'r' && e->vendor && e->off == 0x40 && (e->value & 1));
	}
	unsigned after = starts[count - 1] + 10;
	if (submitted) {
		CHECK(count == 2 && after + 1 == m.pre_event_count);
		const typeof(m.pre_events[0]) *e = &m.pre_events[after];
		CHECK(e->kind == 'w' && !e->vendor && e->off == 0x58 && e->value == 1);
	} else {
		/* Failure can only print and hold, not acknowledge, enable,
		 * perform setup, clear a request, or submit the prepared TUR.
		 */
		for (unsigned i = after; i < m.pre_event_count; i++) { CHECK(m.pre_events[i].kind == 'p'); }
	}
}

static void boundary_failed(unsigned phase, int error)
{
	probe_to_hold();
	boundary_samples(phase);
	diagnostic_samples(0);
	boundary_trace(phase, false);
	CHECK(m.boundary_fault_sent && su.dead && !su.writable && !m.commands && !m.writes && !m.syncs);
	CHECK(!ss.cmd_failures && !ss.timeouts && !ss.retries && !m.clears && !m.command_acks);
	CHECK(ss.last_is == m.boundary_is && ss.is_errors == !!(m.boundary_is & MODEL_IS_ERRORS));
	CHECK(m.mmio_writes == m.boundary_last_writes && m.cache_ops == 0 && m.maps == 1);
	CHECK(!m.r[0x20 / 4]); /* The synthetic IS latch cleared after the sampling read. */
	for (unsigned i = 0; i < 5; i++) { CHECK(!m.r[0x38 / 4 + i]); }
	if (phase == 1) { CHECK(!m.hce_enables && !m.smcs && !m.vendor_writes && !m.base_writes && !su.host_owned); }
	else { CHECK(m.smcs == 1 && m.base_writes == 2 && m.nexus_writes == 1 && su.host_owned); }
	char expected[256];
	snprintf(expected, sizeof(expected), "probe FAILED (%d), writes 0, commands failed 0", error);
	CHECK(occurrences(expected) == 1 && occurrences(" STOP\n") == 1 && occurrences("PROBE HOLD") == 1);
	snprintf(expected, sizeof(expected), "boundary UIC PA %08x DL %08x NL %08x TL %08x DME %08x",
	    m.boundary_uic[0], m.boundary_uic[1], m.boundary_uic[2], m.boundary_uic[3], m.boundary_uic[4]);
	CHECK(occurrences(expected) >= 1);

	/* A test-owned longjmp exits the nonreturning hold before init_error
	 * is assigned. The cached attempt plus permanent su.dead must still
	 * forbid later MMIO and DMA reuse, even after controller-side changes.
	 */
	unsigned accesses = m.mmio_reads + m.mmio_writes, events = m.pre_event_count;
	size_t console_size = m.console_size;
	memset(m.dma + 0x1200, 0x39, 32); /* synthetic late DMA */
	m.r[0x58 / 4] = m.r[0x78 / 4] = 0;
	m.r[0x34 / 4] = 1; m.r[0x30 / 4] = 0x10f;
	uint32_t digest = crc32(0, m.dma, m.size);
	dev_t dev = 0;
	CHECK(sweet_ufs_rootdev(&dev) == EIO && !dev && su.attempted);
	CHECK(ufs_read_capacity() == EIO);
	uint8_t tur[6] = { 0 };
	CHECK(ufs_scsi(tur, sizeof(tur), 0, false) == EIO);
	exynos_ufs_log_boundaries(); /* Per-boot print budget also survives repeated calls. */
	CHECK(accesses == m.mmio_reads + m.mmio_writes && events == m.pre_event_count);
	CHECK(console_size == m.console_size && crc32(0, m.dma, m.size) == digest);
}

static void test_boundaries(void)
{
	for (unsigned enabled = 0; enabled < 2; enabled++) {
		case_name = enabled ? "enabled probe samples pre-enable without restarting HCE" :
		    "two clean probe boundaries with no print between sample and TUR doorbell";
		boundary_probe(); m.r[0x34 / 4] = enabled;
		probe_to_hold();
		boundary_samples(2); diagnostic_samples(0); boundary_trace(2, true);
		CHECK(!su.dead && su.host_owned && !su.writable && !m.writes && !m.syncs);
		CHECK(m.hce_enables == !enabled && m.smcs == 1 && m.commands > 1);
		CHECK(occurrences("probe PASSED (0), writes 0, commands failed 0") == 1);
		CHECK(!occurrences(" STOP\n") && m.uic_reads[0] == 2);
		passed(case_name);
	}

	static const unsigned error_bits[] = { 2, 7, 11, 12, 16, 17, 18 };
	static const uint32_t raw_codes[] = { 1, 0x80000000, 0xdeadbeef };
	for (unsigned phase = 1; phase <= 2; phase++) {
		case_name = "nonerror IS is retained without blocking either boundary";
		boundary_probe(); m.boundary_fault = phase; m.boundary_is = 0x400;
		probe_to_hold();
		boundary_samples(2); diagnostic_samples(0); boundary_trace(2, true);
		CHECK(!su.dead && m.commands && !m.writes && !occurrences(" STOP\n"));
		CHECK(occurrences(phase == 1 ? "boundary pre-enable IS 00000400" : "boundary pre-TUR IS 00000400") == 1);
		passed(case_name);
		for (unsigned i = 0; i < sizeof(error_bits) / sizeof(error_bits[0]); i++) {
			case_name = phase == 1 ? "each sampled IS error stops before enable/FMP" :
			    "each sampled IS error stops before first TUR doorbell";
			boundary_probe(); m.boundary_fault = phase; m.boundary_is = 1u << error_bits[i];
			boundary_failed(phase, EIO);
			passed(case_name);
		}
		for (unsigned layer = 0; layer < 5; layer++) {
			for (unsigned raw = 0; raw < sizeof(raw_codes) / sizeof(raw_codes[0]); raw++) {
				case_name = phase == 1 ? "nonzero pre-enable layer code stops even with IS=0 and no valid bit" :
				    "nonzero pre-TUR layer code stops even with IS=0 and no valid bit";
				boundary_probe(); m.boundary_fault = phase; m.boundary_uic[layer] = raw_codes[raw];
				boundary_failed(phase, EIO);
				passed(case_name);
			}
		}
		for (unsigned task = 0; task < 2; task++) {
			case_name = "pending transfer/task appearing in a boundary prevents the next action";
			boundary_probe(); m.boundary_fault = phase;
			if (task) { m.boundary_tm_dbr = 2; } else { m.boundary_dbr = 4; }
			boundary_failed(phase, EBUSY);
			passed(case_name);
		}
		for (unsigned bit = 0; bit < 4; bit++) {
			case_name = "sampled device/list/UIC readiness loss prevents the next action";
			boundary_probe(); m.boundary_fault = phase; m.boundary_hcs_clear = 1u << bit;
			boundary_failed(phase, ENXIO);
			passed(case_name);
		}
	}

	case_name = "raw mixed layer codes survive clearing and deferred hold output";
	boundary_probe(); m.boundary_fault = 2; m.boundary_is = 0x80000404;
	static const uint32_t codes[] = { 0x80000010, 0x76543210, 0, 0xffffffff, 0x12345678 };
	memcpy(m.boundary_uic, codes, sizeof(codes));
	boundary_failed(2, EIO);
	CHECK(occurrences("boundary pre-enable IS 00000000 HCE 0 HCS 10f DBR 0 TM 0\n") == 1);
	CHECK(occurrences("boundary pre-TUR IS 80000404 HCE 1 HCS 10f DBR 0 TM 0 STOP\n") == 1);
	passed(case_name);

	case_name = "already-enabled pre-enable PA indication quarantines without restart";
	boundary_probe(); m.r[0x34 / 4] = 1; m.boundary_fault = 1; m.boundary_uic[0] = 0x80000010;
	boundary_failed(1, EIO);
	passed(case_name);

	case_name = "sampled HCE loss before TUR prevents submission";
	boundary_probe(); m.boundary_fault = 2; m.boundary_hce_off = true;
	boundary_failed(2, ENXIO);
	passed(case_name);

	for (unsigned scan = 0; scan < 2; scan++) {
		case_name = scan ? "eight-LUN scan samples only the first TUR" : "TUR retries do not repeat pre-submit samples";
		boundary_probe();
		if (scan) { m.disks[0].present = false; }
		else { m.key = 6; m.asc = 0x29; inject(SENSE, 2, 0); }
		probe_to_hold();
		boundary_samples(2); diagnostic_samples(0); boundary_trace(2, true);
		CHECK(!su.dead && !m.writes && m.opcount[0] == (scan ? 8u : 3u));
		CHECK(ss.retries == (scan ? 0u : 2u) && m.uic_reads[0] == 2);
		passed(case_name);
	}

	for (unsigned option = 0; option < 3; option++) {
		case_name = "absent/zero/non-one ufsprobe leaves Exynos boundary sampling disabled";
		reset_model(true, 4096); arg("ufsrw", 1);
		if (option) { arg("ufsprobe", option == 1 ? 0 : 2); }
		m.r[0x38 / 4] = 0x80000010;
		dev_t dev = 0;
		CHECK(sweet_ufs_rootdev(&dev) == 0 && dev && !su.writable && !m.writes);
		boundary_samples(0); diagnostic_samples(0);
		CHECK(m.r[0x38 / 4] == 0x80000010);
		passed(case_name);
	}
	case_name = "Qualcomm ufsprobe=1 adds no boundary sampling or hold";
	reset_model(false, 4096); label(&m.disks[0], "userdata"); arg("ufsprobe", 1); arg("ufsrw", 1);
	m.r[0x38 / 4] = 0x80000010;
	dev_t dev = 0;
	CHECK(sweet_ufs_rootdev(&dev) == 0 && dev && su.writable && !m.smcs && !m.vendor_writes);
	boundary_samples(0); diagnostic_samples(0);
	passed(case_name);

	for (unsigned early = 0; early < 6; early++) {
		case_name = "earlier mapping/readiness/pending/list-stop rejection prevents both boundaries";
		boundary_probe();
		switch (early) {
		case 0: m.node_present = false; break;
		case 1: set_pair("dma-pool", 0, 0); break;
		case 2: m.r[0x30 / 4] &= ~1u; break;
		case 3: m.r[0x58 / 4] = 4; break;
		case 4: m.stop_race = true; break;
		case 5: m.stop_stuck = true; break;
		}
		probe_to_hold();
		boundary_samples(0); diagnostic_samples(0);
		CHECK(!m.commands && !m.hce_enables && !m.smcs && !m.writes);
		passed(case_name);
	}
}

static void test_capacity(void)
{
	static const uint32_t sizes[] = { 512, 1024, 2048, 4096, 0, 256, 513, 8192 };
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		case_name = "READ CAPACITY block-size validation";
		transport(true, 512); m.disks[0].block = sizes[i];
		int e = ufs_read_capacity();
		CHECK(e == (i < 4 ? 0 : EINVAL));
		CHECK(su.device_blocks == (i < 4 ? 0x10000 : 0));
		if (!e) { CHECK(su.block_size == sizes[i]); }
	}
	passed(case_name);
	static const uint64_t blocks[] = { 2, 0x100000000ull, 0x100000100ull, 0, UINT64_MAX / 512 + 1 };
	for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		case_name = "READ CAPACITY(16) and capacity overflow";
		transport(true, 512); m.disks[0].blocks = blocks[i];
		int e = ufs_read_capacity();
		CHECK(e == ((i == 1 || i == 2) ? 0 : EINVAL));
		CHECK(m.opcount[0x9e] == (i != 0));
		if (!e) { CHECK(su.device_blocks == blocks[i]); }
	}
	passed(case_name);
	case_name = "READ(16) at and across 32-bit LBA boundary";
	transport(true, 512); m.disks[0].blocks = 0x100000100ull;
	CHECK(ufs_read_capacity() == 0);
	CHECK(ufs_read_blocks(0xfffffffeull, 1) == 0 && m.last_op == 0x28);
	CHECK(ufs_read_blocks(0xfffffffeull, 3) == 0 && m.last_op == 0x88 && net64(m.last_cdb + 2) == 0xfffffffeull);
	CHECK(ufs_read_blocks(0x100000001ull, 1) == 0 && m.last_op == 0x88);
	passed(case_name);
}

static void test_gpt(void)
{
	for (unsigned block = 512; block <= 4096; block *= 8) {
		case_name = block == 512 ? "512-byte GPT and repeated F2FS sample" : "4096-byte GPT and repeated F2FS sample";
		transport(true, block);
		CHECK(ufs_find_partition("userdata") == 0 && su.first_block == 0x100 && su.nblocks == 0x2000);
		CHECK(ufs_readonly_probe() == 0);
		CHECK(!memcmp(m.dma + 0x4000, m.disks[0].data, 4096) && !m.writes);
		passed(case_name);
	}
	static const char *bad[] = { "header CRC", "entry CRC", "bad header length", "wrong primary LBA",
	    "wrong backup LBA", "inverted usable range", "entry LBA overflow", "entries overlap usable blocks",
	    "entry count overflow", "zero entry size", "non-power-of-two entry size", "partition before usable",
	    "partition beyond usable", "inverted partition", "duplicate root", "non-ASCII suffix", "unused entry" };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		case_name = bad[i];
		transport(true, 512);
		struct disk *d = &m.disks[0];
		switch (i) {
		case 0: d->header[16] ^= 1; break;
		case 1: d->entries[20] ^= 1; break;
		case 2: put_le32(d->header + 12, 91); break;
		case 3: put_le64(d->header + 24, 0); break;
		case 4: put_le64(d->header + 32, 0x10000); break;
		case 5: put_le64(d->header + 40, 0xffff); break;
		case 6: put_le64(d->header + 72, UINT64_MAX); break;
		case 7: put_le64(d->header + 72, 9); break;
		case 8: put_le32(d->header + 80, 0x80000001); break;
		case 9: put_le32(d->header + 84, 0); break;
		case 10: put_le32(d->header + 84, 129); break;
		case 11: put_le64(d->entries + 32, 2); break;
		case 12: put_le64(d->entries + 40, 0xffff); break;
		case 13: put_le64(d->entries + 40, 0xff); break;
		case 14: memcpy(d->entries + 128, d->entries, 128); break;
		case 15: d->entries[56 + 8 * 2 + 1] = 1; break;
		case 16: memset(d->entries, 0, 16); break;
		}
		if (i >= 2 && i <= 10) { crc_header(d); }
		if (i >= 11) { crc_entries(d); }
		uint64_t first = 0, nblocks = 0;
		CHECK(ufs_gpt_find_on_lun("userdata", &first, &nblocks) != 0 && !su.ready && !m.writes);
		if (i >= 2 && i <= 10) { CHECK(m.commands == 1); }
		passed(case_name);
	}
	case_name = "256-byte entries and exact 36-character names";
	transport(true, 4096);
	struct disk *d = &m.disks[0];
	put_le32(d->header + 80, 16); put_le32(d->header + 84, 256);
	label(d, "abcdefghijklmnopqrstuvwxyz0123456789");
	CHECK(ufs_find_partition("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == 0);
	passed(case_name);
	for (unsigned i = 0; i < 2; i++) {
		case_name = i ? "missing second DMA fails sample check" : "changed repeated sample is rejected";
		transport(true, 512);
		CHECK(ufs_find_partition("userdata") == 0);
		inject(i ? MISSING_DATA : CHANGED_DATA, 1, 0x28); m.fault_skip = 1;
		CHECK(ufs_readonly_probe() == EIO);
		passed(case_name);
	}
	case_name = "mixed-geometry LUN scan";
	transport(true, 512);
	m.disks[0].present = false;
	disk_fixture(1, 512); label(&m.disks[1], "SYSTEM");
	disk_fixture(2, 4096);
	CHECK(ufs_find_partition("userdata") == 0 && su.lun == 2 && su.block_size == 4096 && m.visits == 7);
	CHECK(ufs_readonly_probe() == 0);
	passed(case_name);
	case_name = "last normal LUN is scanned";
	transport(true, 512); m.disks[0].present = false; disk_fixture(7, 4096);
	CHECK(ufs_find_partition("userdata") == 0 && su.lun == 7 && m.visits == 255);
	passed(case_name);
	case_name = "no usable LUN";
	transport(true, 512); m.disks[0].present = false;
	CHECK(ufs_find_partition("userdata") != 0 && m.commands == 8 && !su.dead);
	passed(case_name);
}

static void test_handoff(void)
{
	static const char *bad[] = { "missing UFS node", "missing chosen", "wrong UFS compatible",
	    "wrong HCI address", "wrong vendor window", "short pool property", "zero placeholders",
	    "unaligned pool", "pool below Image", "pool overruns Image", "wrong pool size",
	    "Image above DMA32", "Image size wraps", "Image overlaps managed RAM", "RAM wraps", "empty RAM" };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		case_name = bad[i];
		reset_model(true, 4096);
		uint64_t value;
		switch (i) {
		case 0: m.node_present = false; break;
		case 1: m.chosen_present = false; break;
		case 2: property(&m.ufs, "compatible")->raw[4] = 'x'; break;
		case 3: set_pair("reg", 0x1d84000, 0x200); break;
		case 4: set_pair("vendor-reg", 0x11121100, 0x100); break;
		case 5: property(&m.ufs, "dma-pool")->size = 12; break;
		case 6: set_pair("dma-pool", 0, 0); break;
		case 7: set_pair("dma-pool", 0x81100001, 0x104000); break;
		case 8: set_pair("dma-pool", 0x80ffc000, 0x104000); break;
		case 9: set_pair("dma-pool", 0x81200000, 0x104000); break;
		case 10: set_pair("dma-pool", 0x81100000, 0x108000); break;
		case 11: set_pair("loader-image", 0x100000000ull, 0x300000); break;
		case 12: set_pair("loader-image", 0x81000000, UINT64_MAX); break;
		case 13: value = 0x81000000; set_property(&m.chosen, "dram-base", &value, 8); break;
		case 14: value = UINT64_MAX - 0x10000; set_property(&m.chosen, "dram-base", &value, 8); break;
		case 15: value = 0; set_property(&m.chosen, "dram-size", &value, 8); break;
		}
		dev_t dev = 0;
		CHECK(sweet_ufs_rootdev(&dev) != 0 && !m.maps && !m.mmio_reads && !m.mmio_writes && !m.smcs);
		passed(case_name);
	}
	for (unsigned i = 0; i < 3; i++) {
		case_name = "mapping failure before MMIO";
		reset_model(true, 4096);
		m.fail_pool_map = i == 0; m.fail_vendor_map = i == 1; m.fail_hci_map = i == 2;
		dev_t dev = 0;
		CHECK(sweet_ufs_rootdev(&dev) == ENOMEM && !m.mmio_reads && !m.mmio_writes);
	}
	passed(case_name);
	case_name = "unknown board has no storage side effects";
	reset_model(false, 4096); m.unknown_board = true;
	dev_t dev = 0;
	CHECK(sweet_ufs_rootdev(&dev) == ENXIO && !m.allocations && !m.mmio_reads);
	passed(case_name);
}

static void test_init_bsd(void)
{
	for (unsigned block = 512; block <= 4096; block *= 8) {
		case_name = block == 512 ? "512-byte root initialization and BSD reads" : "4096-byte root initialization and BSD reads";
		reset_model(true, block);
		arg("ufsrw", 1); arg("ufsverify", 2); arg("ufscache", 1); arg("ufswait", 1);
		dev_t dev = 0;
		CHECK(sweet_ufs_rootdev(&dev) == 0 && su.ready && dev == makedev(17, 0));
		CHECK(!su.writable && !su.verify && !su.wait && !su.syncsecs && !services);
		CHECK(m.bdevs == 1 && m.cdevs == 1 && m.nodes == 2 && m.maps == 1 && !m.allocations);
		unsigned accesses = m.mmio_reads + m.mmio_writes;
		CHECK(sweet_ufs_rootdev(&dev) == 0 && accesses == m.mmio_reads + m.mmio_writes);
		CHECK(sweet_ufs_open(dev, 0, 0, NULL) == 0);
		CHECK(sweet_ufs_open(dev, FWRITE, 0, NULL) == EROFS);
		CHECK(sweet_ufs_part_size("modem", NULL, NULL) == ENOTSUP);
		CHECK(sweet_ufs_read_part("modem", 0, wire, 1) == ENOTSUP);
		uint32_t n = 0x100000 + 1234;
		uint8_t *data = malloc(n); CHECK(data); memset(data, 0x5a, n);
		struct test_buf b = { .blkno = 1, .count = n, .flags = B_READ, .data = (char *)data };
		unsigned commands = m.commands;
		sweet_ufs_strategy(&b);
		CHECK(b.done && !b.error && !b.resid && !b.mapped && m.commands == commands + 2);
		CHECK(!memcmp(data, m.disks[0].data + 512, n));
		b = (struct test_buf){ .count = 512, .data = (char *)data };
		sweet_ufs_strategy(&b);
		CHECK(b.error == EROFS && b.resid == 512 && !m.writes);
		uint64_t result = UINT64_MAX;
		CHECK(sweet_ufs_ioctl(dev, DKIOCGETPHYSICALBLOCKSIZE, (caddr_t)&result, 0, NULL, 0) == 0 && (uint32_t)result == block);
		CHECK(sweet_ufs_ioctl(dev, DKIOCGETBLOCKCOUNT, (caddr_t)&result, 0, NULL, 0) == 0 && result == 0x2000ull * block / 512);
		CHECK(sweet_ufs_ioctl(dev, DKIOCGETMAXBLOCKCOUNTWRITE, (caddr_t)&result, 0, NULL, 0) == 0 && result == 0);
		CHECK(sweet_ufs_ioctl(dev, DKIOCSYNCHRONIZECACHE, NULL, 0, NULL, 0) == 0 && !m.syncs);
		uint32_t size = block;
		CHECK(sweet_ufs_ioctl(dev, DKIOCSETBLOCKSIZE, (caddr_t)&size, 0, NULL, 1) == ENODEV);
		CHECK(sweet_ufs_ioctl(dev, DKIOCSETBLOCKSIZE, (caddr_t)&size, 0, NULL, 0) == 0 && su.secsize == block);
		size = block * 2;
		CHECK(sweet_ufs_ioctl(dev, DKIOCSETBLOCKSIZE, (caddr_t)&size, 0, NULL, 0) == EINVAL);
		free(data);
		passed(case_name);
	}
	case_name = "BSD bounds, block-number overflow and partial failure residual";
	uint8_t *data = malloc(0x100200); CHECK(data);
	unsigned commands = m.commands;
	int64_t bad[] = { -1, INT64_MAX, (int64_t)(UINT64_MAX / su.secsize + 1), (int64_t)su.nblocks };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		struct test_buf b = { .blkno = bad[i], .count = 512, .flags = B_READ, .data = (char *)data };
		sweet_ufs_strategy(&b); CHECK(b.error == EINVAL && b.resid == 512 && b.done);
	}
	CHECK(m.commands == commands);
	struct test_buf b = { .blkno = su.nblocks, .flags = B_READ };
	sweet_ufs_strategy(&b); CHECK(!b.error && b.done);
	b = (struct test_buf){ .count = 512, .flags = B_READ, .fail_map = true };
	sweet_ufs_strategy(&b); CHECK(b.error == EIO && b.resid == 512);
	b = (struct test_buf){ .count = 0x100200, .flags = B_READ, .data = (char *)data };
	memset(data, 0xa6, b.count);
	inject(RESIDUAL, 1, 0x28); m.fault_skip = 1;
	sweet_ufs_strategy(&b);
	CHECK(b.error == EIO && b.resid == 512 && !memcmp(data, m.disks[0].data, 0x100000));
	for (unsigned i = 0x100000; i < b.count; i++) { CHECK(data[i] == 0xa6); }
	free(data);
	passed(case_name);

	static const char *holds[] = { "successful probe holds before BSD registration",
	    "HCE timeout probe holds", "early handoff failure holds",
	    "post-enable readiness failure holds", "post-enable host error holds" };
	for (unsigned i = 0; i < sizeof(holds) / sizeof(holds[0]); i++) {
		case_name = holds[i];
		reset_model(true, 4096); arg("ufsprobe", 1); m.expect_hold = true;
		if (i == 1) { m.hce_stuck = true; }
		if (i == 2) { set_pair("dma-pool", 0, 0); }
		if (i == 3) { m.hce_hcs &= ~1u; }
		if (i == 4) { m.hce_is = 0x10000; }
		if (!setjmp(hold)) { dev_t dev; (void)sweet_ufs_rootdev(&dev); CHECK(false); }
		CHECK(!su.ready && !m.bdevs && !m.cdevs && !m.nodes && !m.writes && !services && !sweet_ufs_lock);
		passed(case_name);
	}
}

static void test_qualcomm(void)
{
	case_name = "Qualcomm 16-byte PRDT, write verification and partial RMW";
	reset_model(false, 4096); label(&m.disks[0], "userdata");
	arg("ufsrw", 1); arg("ufsverify", 2); arg("ufscache", 1);
	dev_t dev = 0;
	CHECK(sweet_ufs_rootdev(&dev) == 0 && su.writable && services == 13);
	CHECK(m.allocations == 1 && m.size == 0x204000 && !m.maps && !m.smcs && !m.vendor_writes && m.cache_ops);
	uint8_t before[8192]; memcpy(before, m.disks[0].data, sizeof(before));
	uint8_t data[4096]; memset(data, 0x5b, sizeof(data));
	struct test_buf b = { .blkno = 1, .count = sizeof(data), .data = (char *)data };
	sweet_ufs_strategy(&b);
	CHECK(!b.error && !b.resid && m.writes == 1 && m.syncs == 1 && ss.verify_ok == 1 && ss.strat_rmw == 1);
	CHECK(!memcmp(m.disks[0].data, before, 512));
	CHECK(!memcmp(m.disks[0].data + 512, data, 4096));
	CHECK(!memcmp(m.disks[0].data + 4608, before + 4608, sizeof(before) - 4608));
	CHECK(ufs_read_blocks(0x100, 256) == 0 && m.last_prds == 4);
	CHECK(!memcmp(m.dma + 0x4000, m.disks[0].data, 0x100000));
	passed(case_name);

	case_name = "Qualcomm partition write bounds and LBA overflow";
	unsigned writes = m.writes;
	CHECK(ufs_write_blocks(0xff, 1) == EPERM);
	CHECK(ufs_write_blocks(su.first_block + su.nblocks - 1, 2) == EPERM);
	su.first_block = UINT64_MAX - 1; su.nblocks = 2;
	CHECK(ufs_write_blocks(UINT64_MAX, 1) == EPERM);
	CHECK(ufs_write_blocks(UINT64_MAX - 1, 2) == EPERM && m.writes == writes);
	su.first_block = 0x100; su.nblocks = 0x2000;
	passed(case_name);

	case_name = "Qualcomm auxiliary LUN lookup restores root and caches bounds";
	disk_fixture(1, 4096); label(&m.disks[1], "modem");
	uint8_t lun = 255; uint64_t bytes = 0;
	CHECK(sweet_ufs_part_size("modem", &lun, &bytes) == 0 && lun == 1 && bytes == 0x2000000);
	unsigned commands = m.commands;
	CHECK(sweet_ufs_part_size("modem", &lun, &bytes) == 0 && commands == m.commands && su.lun == 0);
	CHECK(sweet_ufs_read_part("modem", 17, data, sizeof(data)) == 0 && su.lun == 0);
	CHECK(!memcmp(data, m.disks[1].data + 17, sizeof(data)));
	passed(case_name);

	case_name = "Qualcomm legacy case/CRC policy";
	transport(false, 4096);
	uint64_t first, blocks;
	CHECK(ufs_gpt_find_on_lun("userdata", &first, &blocks) == ENOENT);
	label(&m.disks[0], "userdata"); m.disks[0].header[16] ^= 1; m.disks[0].header[88] ^= 1;
	CHECK(ufs_gpt_find_on_lun("userdata", &first, &blocks) == 0);
	passed(case_name);

	case_name = "Qualcomm timeout-clear polarity and retry";
	transport(false, 4096); su.wait = su.ready = true;
	inject(TIMEOUT, 1, 0x28);
	CHECK(ufs_read_blocks(0x100, 1) == 0 && m.clear_value == ~1u && ss.retries == 1 && m.sleeps && !su.dead);
	passed(case_name);
	case_name = "Qualcomm unresolved timeout is quarantined";
	transport(false, 4096); m.sticky_dbr = true; inject(TIMEOUT, 1, 0x28);
	CHECK(ufs_read_blocks(0x100, 1) == ETIMEDOUT && su.dead && ss.retries == 0);
	passed(case_name);

	for (unsigned cap = 0; cap < 2; cap++) {
		case_name = cap ? "Qualcomm DMA64 capability" : "Qualcomm DMA64 rejected without capability";
		reset_model(false, 4096); label(&m.disks[0], "userdata");
		m.allocation_pa = 0x101000000ull;
		if (!cap) { m.r[0] &= ~0x1000000u; }
		CHECK(sweet_ufs_rootdev(&dev) == (cap ? 0 : ENXIO));
		CHECK((m.commands > 0) == !!cap);
		passed(case_name);
	}
	case_name = "Qualcomm verify allocation falls back";
	reset_model(false, 4096); label(&m.disks[0], "userdata");
	arg("ufsverify", 2); m.fail_verify_alloc = true;
	CHECK(sweet_ufs_rootdev(&dev) == 0 && m.allocations == 2 && su.verify == 0 && m.size == 0x104000);
	passed(case_name);
	guards();
}

int main(int argc, char **argv)
{
	CHECK(argc == 3);
	for (unsigned i = 0; i < 2; i++) {
		FILE *f = fopen(argv[i + 1], "rb"); CHECK(f);
		unsigned size = (i ? 4096 : 512) + 8192 + 4096;
		CHECK(fread(fixture_data[i], 1, size, f) == size && fgetc(f) == EOF);
		fclose(f);
	}
	test_transport();
	test_enable();
	test_takeover();
	test_failures();
	test_diagnostics();
	test_boundaries();
	test_preenable_linereset();
	test_link_startup();
	test_uic_failures();
	test_nop();
	test_capacity();
	test_gpt();
	test_handoff();
	test_init_bsd();
	test_qualcomm();
	free(m.allocation);
	for (unsigned i = 0; i < 8; i++) { free(m.disks[i].data); }
	printf("PASS UFS host: %u scenarios, byte-level protocol/DMA checks, ASan/UBSan\n", cases);
	return 0;
}
