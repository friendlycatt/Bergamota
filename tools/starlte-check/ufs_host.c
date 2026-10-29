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
static void model_wr(uint32_t, uint32_t, bool);
static uint64_t model_smc(void);
static void model_barrier(void);
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

#include "ufs-production.inc"

/* Deliberately independent of struct utrd/ucd and their layout constants. */
#define MODEL_WINDOW 0x204000u
#define MODEL_DATA (3u << 20)
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
};
struct model {
	bool exynos, unknown_board, node_present, chosen_present;
	bool hci_mapped, vendor_mapped, pool_mapped;
	bool sticky_dbr, stop_stuck, tm_stop_stuck, stop_race, base_stuck, start_stuck;
	bool prdt_stuck, nexus_stuck, fail_hci_map, fail_vendor_map, fail_pool_map;
	bool fail_alloc, fail_verify_alloc, expect_hold, waiting_hold;
	uint64_t smc_result, now, wake, pa, allocation_pa;
	uint32_t r[128], v[128], size, clear_value, visits;
	unsigned mmio_reads, mmio_writes, vendor_writes, commands, writes, syncs;
	unsigned base_writes, allocations, maps, smcs, barriers, cache_ops, sleeps;
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
} m;
static uint8_t fixture_data[2][4096 + 8192 + 4096];
static uint8_t wire[1u << 20];
static jmp_buf hold;

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
	m.allocation_pa = exynos ? 0x81100000ull : 0xb2000000ull;
	CHECK(posix_memalign((void **)&m.allocation, 0x4000, MODEL_WINDOW + 0x8000) == 0);
	memset(m.allocation, 0xa7, MODEL_WINDOW + 0x8000);
	m.dma = m.allocation + 0x4000;
	m.r[0x34 / 4] = 1;
	m.r[0x30 / 4] = 3;
	m.r[0x00 / 4] = exynos ? 31 : 0x100001f;
	m.r[0x08 / 4] = 0x210;
	m.r[0x50 / 4] = 0x82000000;
	m.r[0x60 / 4] = m.r[0x80 / 4] = 1;
	m.v[0x00 / 4] = 0x8000000c;
	m.v[0x04 / 4] = 12;
	m.v[0x40 / 4] = 0xa5000010;
	set_pair("reg", exynos ? 0x11120000 : 0x1d84000, exynos ? 0x200 : 0x1000);
	set_pair("vendor-reg", 0x11121100, 0x200);
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
	uint64_t list = m.r[0x50 / 4] | (uint64_t)m.r[0x54 / 4] << 32;
	CHECK(!(list & 1023));
	uint8_t *t = dma_at(list, 32);
	uint64_t cmd_pa = le64(t + 16);
	CHECK(!(cmd_pa & 127));
	uint8_t *cmd = dma_at(cmd_pa, 32);
	uint32_t len = net32(cmd + 12), direction = (le32(t) >> 25) & 3;
	CHECK(len <= sizeof(wire));
	CHECK(le32(t) == (0x11000000u | direction << 25));
	CHECK(le32(t + 4) == 0 && le32(t + 8) == 15 && le32(t + 12) == 0);
	CHECK(cmd[0] == 1 && cmd[1] == (len ? (direction == 1 ? 0x20 : 0x40) : 0));
	CHECK(direction == (len ? ((cmd[16] == 0x2a) ? 1 : 2) : 0));
	CHECK(cmd[2] < 8 && cmd[4] == 0 && net32(cmd + 8) == 0);
	unsigned unit = m.exynos ? 1 : 4;
	unsigned resp_offset = le16(t + 26) * unit, resp_size = le16(t + 24) * unit;
	unsigned prdt_offset = le16(t + 30) * unit, entries = le16(t + 28);
	unsigned stride = m.exynos ? 128 : 16;
	if (m.exynos) {
		CHECK(m.smcs == 1 && m.smc_result == 0 && cmd[3] == 0);
		CHECK(m.v[0x40 / 4] & 1);
		CHECK((m.v[0] & 31) == 12 && (m.v[1] & 31) == 12);
		CHECK(entries % 128 == 0);
		entries /= 128;
		/* Force the next command to set the Nexus bit again. */
		m.v[0x40 / 4] &= ~1u;
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
	m.last_prds = entries;
	m.last_len = len;
	m.last_op = cmd[16];
	m.opcount[cmd[16]]++;
	m.visits |= 1u << cmd[2];
	memcpy(m.last_cdb, cmd + 16, 16);
	enum fault fault = OK;
	if (m.fault_left && (m.fault_op < 0 || m.fault_op == cmd[16])) {
		if (m.fault_skip) { m.fault_skip--; }
		else { fault = m.fault; m.fault_left--; }
	}
	if (fault == TIMEOUT) {
		m.pending_pa = entries ? spans[0].pa : cmd_pa + resp_offset;
		m.pending_len = entries ? spans[0].len : 32;
		return;
	}
	memset(resp, 0, 52);
	resp[0] = 0x21;
	resp[2] = cmd[2];
	resp[3] = cmd[3];
	put_le32(t + 8, 0);
	struct disk *d = &m.disks[cmd[2]];
	if (!d->present || fault == SENSE) {
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
	case BAD_TYPE: resp[0] = 0x31; break;
	case BAD_LUN: resp[2] ^= 1; break;
	case BAD_TAG: resp[3] ^= 1; break;
	case BAD_RESPONSE: resp[6] = 1; break;
	case HOST_ERROR: m.r[0x20 / 4] |= 0x10000; break;
	case RESIDUAL: put_net32(resp + 12, 4); break;
	case UNDERFLOW: resp[1] = 0x20; break;
	case OVERFLOW: resp[1] = 0x40; break;
	default: break;
	}
	m.r[0x20 / 4] |= 1;
	m.r[0x58 / 4] = 0;
}

static uint32_t model_rd(uint32_t off, bool vendor)
{
	CHECK((vendor ? m.vendor_mapped : m.hci_mapped) && off < 0x200 && !(off & 3));
	m.mmio_reads++;
	return (vendor ? m.v : m.r)[off / 4];
}

static void model_wr(uint32_t off, uint32_t value, bool vendor)
{
	CHECK((vendor ? m.vendor_mapped : m.hci_mapped) && off < 0x200 && !(off & 3));
	m.mmio_writes++;
	if (vendor) {
		CHECK(m.exynos);
		m.vendor_writes++;
		if (m.prdt_stuck && (off == 0 || off == 4)) { return; }
		if (m.nexus_stuck && off == 0x40) { return; }
		m.v[off / 4] = value;
		return;
	}
	if (off == 0x20) { m.r[off / 4] &= ~value; return; }
	if (off == 0x5c) {
		m.clear_value = value;
		CHECK(value == (m.exynos ? 1u : ~1u));
		if (!m.sticky_dbr) { m.r[0x58 / 4] = 0; }
		return;
	}
	if (off == 0x60 && !value && m.stop_stuck) { return; }
	if (off == 0x80 && !value && m.tm_stop_stuck) { return; }
	if (off == 0x60 && !value && m.stop_race) { m.r[0x58 / 4] = 2; }
	if (off == 0x60 && value && m.start_stuck) { return; }
	if (off == 0x50 || off == 0x54) {
		CHECK(m.r[0x60 / 4] == 0);
		m.base_writes++;
		if (m.base_stuck) { return; }
	}
	m.r[off / 4] = value;
	if (off == 0x58) { model_command(); }
}

static uint64_t model_smc(void)
{
	CHECK(m.exynos && !m.r[0x60 / 4] && !m.r[0x80 / 4]);
	CHECK(!m.r[0x58 / 4] && !m.r[0x78 / 4]);
	m.smcs++;
	return m.smc_result;
}
static void model_barrier(void) { m.barriers++; }
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
static uint64_t mach_absolute_time(void) { m.now += 1000000; return m.now; }
static void delay(int us) { CHECK(us >= 0); m.now += (uint64_t)us * 1000; }
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

static void test_takeover(void)
{
	static const char *names[] = { "disabled HCE", "missing device", "host not ready",
	    "inherited transfer", "inherited task", "transfer list will not stop",
	    "task list will not stop", "request arrives during stop", "SMC rejected",
	    "PRDT setup rejected", "list base rejected", "list start rejected" };
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		case_name = names[i];
		reset_model(true, 4096);
		switch (i) {
		case 0: m.r[0x34 / 4] = 0; break;
		case 1: m.r[0x30 / 4] = 2; break;
		case 2: m.r[0x30 / 4] = 1; break;
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

	for (unsigned i = 0; i < 3; i++) {
		case_name = i == 0 ? "successful probe holds before BSD registration" :
		    i == 1 ? "failed host probe holds" : "early handoff failure holds";
		reset_model(true, 4096); arg("ufsprobe", 1); m.expect_hold = true;
		if (i == 1) { m.r[0x34 / 4] = 0; }
		if (i == 2) { set_pair("dma-pool", 0, 0); }
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
	test_takeover();
	test_failures();
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
