/* Host adapters for the actual Enxerto loader/FDT/inflater. No device access. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "bootui.h"
#include "charger.h"
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "mmu.h"

static jmp_buf handoff;
uint64_t captured_entry, captured_args;
unsigned int charger_handoffs;
char diagnostics[8192];

const char *test_boot(uintptr_t fdt)
{
	captured_entry = captured_args = 0;
	charger_handoffs = 0;
	if (setjmp(handoff)) return NULL;
	return loader_boot(fdt);
}

int mmu_enable_ranges(struct mmu_info *info, const struct mmu_range *ranges, int n)
{
	(void)ranges;
	(void)n;
	memset(info, 0, sizeof(*info));
	return 0;
}

void mmu_off_jump(uint64_t entry, uint64_t args, const struct mmu_range ranges[3])
{
	(void)ranges;
	captured_entry = entry;
	captured_args = args;
	longjmp(handoff, 1);
}

void enxerto_charger_handoff(void) { charger_handoffs++; }
void bootui_init(void) {}
void bootui_handoff(void) {}
void bootui_progress(unsigned int value, const char *label)
{
	(void)value; (void)label;
}
void bootui_mib(const char *label, uint64_t done, uint64_t total, unsigned int lo, unsigned int hi)
{
	(void)label; (void)done; (void)total; (void)lo; (void)hi;
}
void con_hex(const char *label, uint64_t value) { (void)label; (void)value; }
void con_hex2(const char *label, const char *a, uint32_t av, const char *b, uint32_t bv)
{
	(void)label; (void)a; (void)av; (void)b; (void)bv;
}
void con_str(const char *label, const char *value)
{
	size_t offset = strlen(diagnostics);
	snprintf(diagnostics + offset, sizeof(diagnostics) - offset, "%s: %s\n", label, value);
}
