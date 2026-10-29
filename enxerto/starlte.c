/* Galaxy S9 initial handoff: DECON refresh + framebuffer + FDT + SWEETXNU loader.
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

/* Samsung dpu_9810/regs-{decon,dpp}.h; pinned sources in docs/STARLTE.md. */
#define DECON_BASE		0x16030000UL
#define DECON_GLOBAL_CONTROL	0x0000
#define DECON_TRIGGER		0x0070
#define DECON_TE_COUNT		0x0078
#define DECON_WIN_ENABLE		0x0214
#define DECON_WIN_CHANNELS	0x0218
#define DECON_FRAME_COUNT	0x02a0
#define DECON_SHADOW		0x7000
#define DPU_DMA_BASE		0x16070000UL
#define IDMA_IN_CON		0x0008
#define IDMA_SRC_SIZE		0x0010
#define IDMA_SRC_OFFSET		0x0014
#define IDMA_BASE_Y		0x0040

extern char _head[], _end[];
static const char *stage;

static uint32_t read32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static uint32_t decon_read(unsigned offset)
{
	return read32(DECON_BASE + offset);
}

static void park(void) __attribute__((noreturn));
static void park(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
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

static void display_info(uint32_t trigger_before, uint32_t trigger_after, uint32_t frames_before)
{
	con_hex2("TRIGGER", "old=", trigger_before, "new=", trigger_after);
	con_hex2("DECON", "ctrl=", decon_read(DECON_GLOBAL_CONTROL),
		 "te=", decon_read(DECON_TE_COUNT));
	con_hex2("WINDOWS", "reg=", decon_read(DECON_WIN_ENABLE),
		 "live=", decon_read(DECON_SHADOW + DECON_WIN_ENABLE));
	con_hex2("CHANNELS", "reg=", decon_read(DECON_WIN_CHANNELS),
		 "live=", decon_read(DECON_SHADOW + DECON_WIN_CHANNELS));
	/* Raw DMA addresses may be IOVAs. Report them; do not redirect RAM writes
	 * to an unverified address or reprogram the inherited scanout.
	 */
	for (unsigned i = 0; i < 6; i++) {
		uintptr_t dma = DPU_DMA_BASE + (i + 1) * 0x1000;
		char label[] = "IDMA0";
		label[4] += i;
		con_hex2(label, "base=", read32(dma + IDMA_BASE_Y),
			 "src=", read32(dma + IDMA_SRC_SIZE));
		con_hex2(label, "con=", read32(dma + IDMA_IN_CON),
			 "off=", read32(dma + IDMA_SRC_OFFSET));
	}
	con_hex2("FRAMES", "old=", frames_before, "now=", decon_read(DECON_FRAME_COUNT));
}

void enxerto_main(uint64_t dtb, uint64_t entry_el, uint64_t entry_sctlr)
{
	/* Leave an early cyan strip if the inherited panel already refreshes. */
	stage = "entry / FDT identification";
	con_rect(0, 0, FB_W, 64, 0xff00ffffU);
	__asm__ volatile("dsb sy" ::: "memory");
	int exynos = fdt_root_compatible(dtb, "Samsung,EXYNOS9810") ||
		fdt_root_compatible(dtb, "samsung,exynos9810");
	uint32_t trigger_before = 0, trigger_after = 0, frames_before = 0;
	if (exynos) {
		stage = "DECON trigger";
		trigger_before = decon_read(DECON_TRIGGER);
		frames_before = decon_read(DECON_FRAME_COUNT);
		/* Mu-Silicium 7a2ada1, S5E9810 PlatformSec.c: enable inherited
		 * framebuffer refresh. This is the only display-register write.
		 */
		*(volatile uint32_t *)(DECON_BASE + DECON_TRIGGER) = 0x1281;
		__asm__ volatile("dsb sy" ::: "memory");
		trigger_after = decon_read(DECON_TRIGGER);
	}
	stage = "entry diagnostics";
	con_clear(BG_BOOT);
	con_set_scale(2);
#if STARLTE_DISPLAY_PROBE
	con_str("ENXERTO", "DISPLAY PROBE 1");
	con_str("MODE", "early diagnostics; XNU loading disabled");
#else
	con_str("ENXERTO", "Galaxy S9 / Exynos9810 bring-up");
#endif
	con_hex2("EL", "in=", entry_el >> 2, "now=", rsr("CurrentEL") >> 2);
	con_hex("SCTLRin", entry_sctlr);
	con_hex("SCTLR", rsr("sctlr_el1"));
	con_hex("MPIDR", rsr("mpidr_el1"));
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));
	con_hex("PFR0", rsr("id_aa64pfr0_el1"));
	con_hex("CNTFRQ", rsr("cntfrq_el0"));
	con_hex("IMAGE", (uintptr_t)_head);
	con_hex("IMG END", (uintptr_t)_end);
	con_hex("DTB", dtb);
	con_hex("FB", FB_BASE);
	if (exynos) {
		stage = "DECON / IDMA readback";
		display_info(trigger_before, trigger_after, frames_before);
	} else {
		con_str("DECON", "skipped: non-Exynos FDT");
	}
	stage = "FDT diagnostics";
	uint64_t start, end;
	if (fdt_chosen_u64(dtb, "linux,initrd-start", &start))
		con_hex("INITRD", start);
	else
		con_str("INITRD", "missing linux,initrd-start");
	if (fdt_chosen_u64(dtb, "linux,initrd-end", &end))
		con_hex("RD END", end);
	else
		con_str("RD END", "missing linux,initrd-end");
	fdt_board_info(dtb);
#if STARLTE_DISPLAY_PROBE
	con_rect(0, 0, FB_W / 3, 64, 0xffff0000U);
	con_rect(FB_W / 3, 0, FB_W / 3, 64, 0xff00ff00U);
	con_rect(2 * FB_W / 3, 0, FB_W / 3, 64, 0xff0000ffU);
	stage = "display probe parked";
	con_str("PROBE", "READY - photograph this screen");
	park();
#endif
	stage = "XNU loader";
	const char *why = loader_boot(dtb);
	if (bootui_active()) bootui_fail(why);
	con_str("LOADER", why);
	con_str("STARLTE", "handoff stopped; photograph these diagnostics");
	park();
}
