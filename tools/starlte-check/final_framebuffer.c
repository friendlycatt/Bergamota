/* Include the complete production translation unit so its static mapping
 * helper and phystokv execute with real XNU headers and assertions.
 * The VM allocator, final root and TTBR transitions below are test-owned.
 */
#include <arm64/arm_vm_init.c>

extern tt_entry_t test_kernel_root[];
static tt_entry_t test_fb_l2[ARM_PGBYTES / sizeof(tt_entry_t)] __attribute__((aligned(ARM_PGBYTES)));
static tt_entry_t test_invalid_root[ARM_PGBYTES / sizeof(tt_entry_t)] __attribute__((aligned(ARM_PGBYTES)));
tt_entry_t *cpu_tte;
uint64_t test_final_report[12];
unsigned char iso_font[256 * 16];
STATIC_IF_KEY_DEFINE_TRUE(mach_assert);

/* Unused panic service: any call leaves an exception in the QEMU log. */
void
(panic)(const char *format, ...)
{
	(void)format;
	__asm__ volatile("brk #1");
	for (;;) __asm__ volatile("wfe");
}

static uint64_t
fixture_pa(const void *pointer)
{
	return (uintptr_t)pointer - gVirtBase + gPhysBase;
}

static uint64_t
translate(uint64_t va)
{
	uint64_t par;
	__asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
	return par;
}

void
test_final_framebuffer(boot_args *args)
{
	gVirtBase = args->virtBase;
	gPhysBase = args->physBase;
	real_phys_size = args->memSize;
	cpu_tte = test_kernel_root;
	const tt_entry_t *boot_root = (const tt_entry_t *)(bootstrap_pagetables + ARM_PGBYTES);
	for (unsigned int i = 0; i < ARM_PGBYTES / sizeof(tt_entry_t); i++) {
		cpu_tte[i] = boot_root[i];
	}
	const uint64_t va = SWEET_EARLY_FB_VA;
	const uint64_t fb = args->Video.v_baseAddr;
	const uint64_t size = args->Video.v_rowBytes * args->Video.v_height;
	const unsigned int index = L1_TABLE_T1_INDEX(va, TCR_EL1_BOOT);
	assert(index != L1_TABLE_T1_INDEX(gVirtBase, TCR_EL1_BOOT));
	cpu_tte[index] = fixture_pa(test_fb_l2) | ARM_TTE_VALID | ARM_TTE_TYPE_TABLE;
	sweet_early_set_video(fb, args->Video.v_width, args->Video.v_height, args->Video.v_rowBytes);
	sweet_early_set_cmdline("sweetearlyfb=1");
	sweet_early_map_final();
	const uint64_t root_pa = fixture_pa(cpu_tte);
	const uint64_t empty_pa = fixture_pa(test_invalid_root);
	__asm__ volatile("dsb ishst\n\tmsr ttbr1_el1, %0\n\tisb" :: "r"(root_pa) : "memory");
	sweet_early_relocate(va);
	__asm__ volatile("msr ttbr0_el1, %0\n\tisb\n\tdsb ish\n\ttlbi vmalle1\n\tdsb ish\n\tisb"
	    :: "r"(empty_pa) : "memory");
	/* This production console write now has only the final alias available. */
	sweet_early_mark(10, "fixture TTBR0 removed");
	const uint64_t alias = va + (fb & (ARM_TT_L2_SIZE - 1));
	*(volatile uint32_t *)(uintptr_t)alias = 0x89abcdef;
	*(volatile uint32_t *)(uintptr_t)(alias + size - 4) = 0xfedcba98;
	test_final_report[0] = 0x4642;
	test_final_report[1] = va;
	test_final_report[2] = sweet_early_framebuffer_block();
	test_final_report[3] = translate(fb);
	test_final_report[4] = translate(alias);
	test_final_report[5] = translate(alias + size - 4);
	__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(test_final_report[6]));
	__asm__ volatile("mrs %0, ttbr1_el1" : "=r"(test_final_report[7]));
	test_final_report[8] = test_fb_l2[(va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT];
	test_final_report[9] = alias;
	test_final_report[10] = size;
	__asm__ volatile("dsb sy" ::: "memory");
	test_final_report[11] = 0x5041;
}
