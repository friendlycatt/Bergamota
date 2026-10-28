#pragma once
/*
 * Board selection: build.sh passes BOARD_BERYLLIUM or BOARD_STARLTE;
 * the default is sweet (SM7150). starlte provenance: docs/STARLTE.md.
 * Constants come from board device trees and firmware-port references.
 */
#if defined(BOARD_STARLTE)
#define BOARD_NAME		"starlte"
/* Mu-Silicium MemoryMapLib.c + starlte.dsc; confirm the live scanout. */
#define FB_BASE			0xcc000000UL
#define FB_W			1440
#define FB_H			2960
#define GICD_BASE		0x10101000UL
#define GICC_BASE		0x10102000UL
#elif defined(BOARD_BERYLLIUM)
#define BOARD_NAME		"beryllium"
/* reserved-memory cont_splash_mem@9d400000, simple-framebuffer 1080x2246 */
#define FB_BASE			0x9d400000UL
#define FB_W			1080
#define FB_H			2246
/* sdm845 imem@146bf000, restart_reason@65c */
#define IMEM_RESTART_REASON	0x146bf65cUL
#else
#define BOARD_NAME		"sweet"
/* reserved-memory cont_splash_region@9c000000 */
#define FB_BASE			0x9c000000UL
#define FB_W			1080
#define FB_H			2400
/* qcom,msm-imem restart_reason@65c */
#define IMEM_RESTART_REASON	0x146aa65cUL
#endif

#define FB_STRIDE		(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */
#define FB_PHYS			FB_BASE

/* Same on SM7150 and SDM845 */
#if !defined(BOARD_STARLTE)
#define PSHOLD			0x0c264000UL
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define UFS_BASE		0x01d84000UL	/* ufshc@1d84000 */
#endif
