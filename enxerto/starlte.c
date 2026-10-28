/* Galaxy S9 initial handoff: inherited framebuffer + FDT + SWEETXNU loader.
 * Source-derived addresses are in board.h; live validation is still required.
 * This translation unit is selected instead of the Qualcomm platform code.
 */
#include <stdint.h>
#include "board.h"
#include "bootui.h"
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define rsr(r) ({ uint64_t v; __asm__ volatile("mrs %0, " r : "=r"(v)); v; })
static const char *stage;

static void park(void) __attribute__((noreturn));
static void park(void)
{
	for (;;) __asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	con_clear(BG_PANIC);
	con_set_scale(2);
	con_str("STARLTE", stage ? stage : "entry");
	con_hex("SLOT", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	park();
}

void secondary_main(uint64_t cpu)
{
	(void)cpu;
	park();
}

void enxerto_main(uint64_t dtb)
{
	stage = "FDT diagnostics";
	con_clear(BG_BOOT);
	con_set_scale(2);
	con_str("ENXERTO", "Galaxy S9 / Exynos9810 bring-up");
	con_hex("EL", rsr("CurrentEL") >> 2);
	con_hex("MPIDR", rsr("mpidr_el1"));
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));
	con_hex("PFR0", rsr("id_aa64pfr0_el1"));
	con_hex("CNTFRQ", rsr("cntfrq_el0"));
	con_hex("DTB", dtb);
	con_hex("FB", FB_BASE);
	fdt_board_info(dtb);
	stage = "XNU loader";
	const char *why = loader_boot(dtb);
	if (bootui_active()) bootui_fail(why);
	con_str("LOADER", why);
	con_str("STARLTE", "handoff stopped; photograph these diagnostics");
	park();
}
