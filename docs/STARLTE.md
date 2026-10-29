# Galaxy S9 / Exynos9810 bring-up

Target: Samsung Galaxy S9 **starlte** (Exynos9810, SM-G960F-class hardware),
using Bergamota's iOS 26 / XNU12377 stack. The Snapdragon S9 and S9+ have
different board descriptions.

**Status, 2026-10-29:** early platform components pass the Linux/QEMU checks
below. A full kernel containing the S9 changes has yet to be built or booted;
the user's S9 has yet to be identified and tested. Samsung storage, touch,
Mali GPU, USB and power-management support remain follow-up work.

The port reuses Heartwood, Renegade's arm64e/PAC rewriting, and the kernel PAC
fallback in patch 1019. The new kernel work is
[`1131-starlte-exynos9810-bringup.patch`](../xnu/patches-12377/1131-starlte-exynos9810-bringup.patch),
applied after the existing series with `MACHINE_CONFIG=SWEET`.
The active handoff and local artifact paths are in [`NOTES.md`](../NOTES.md).

## Pinned hardware references

* [Samsung kernel reference][samsung]: `duhansysl/exynos9810-kernel` at
  `914eabe7b6799ab0eb159360a5f9731dd50d0f1d`. The supplied checkout is
  `/home/friendlycatt/exynos9810-kernel`; the primary description is
  `arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts`.
* [Mu-Silicium][mu]: `Project-Silicium/Mu-Silicium` at
  `7a2ada19841a129f86fb791c0b9a710436c849a4`. Its starlte firmware-port
  sources provide the framebuffer and upper-bank RAM references.

These are public reference-device facts. The constants in
[`tools/mkadt.py`](../tools/mkadt.py) and [`enxerto/board.h`](../enxerto/board.h)
still need comparison with the live device.

| Item | Current value | Source |
| --- | --- | --- |
| GIC | GICv2; GICD `0x10101000 + 0x1000`, GICC `0x10102000 + 0x1000` | [Samsung DTS][samsung], lines 5263–5270; `arm,cortex-a15-gic`, `arm,cortex-a9-gic` |
| CPU affinities | `0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103` | Samsung DTS, `/cpus`, lines 5006–5104; `arm,ananke` / `arm,meerkat` |
| CPU_ON | PSCI SMC, function `0xc4000003` | Samsung DTS, `/psci`, lines 5232–5238 |
| Counter / timer | 26 MHz; virtual timer INTID 27 | Samsung DTS, `/timer`, lines 5272–5279 |
| Candidate XNU RAM | `0x880000000 + 0x80000000` (2 GiB) | [Mu `Resources/DTBs/starlte.dts`][mu-dts], lines 14606–14608: `reg = <0x08 0x80000000 0x80000000>`, with root address/size cells 2/1 |
| Framebuffer reservation | `0xcc000000 + 0x01043000` | [Mu `MemoryMapLib.c`][mu-memory] |
| Scanout geometry | 1440×2960, 32-bit; assumed tightly packed stride 5760 bytes | [Mu `starlte.dsc`][mu-board] |
| Nominal CPU metadata | LITTLE 1,794,000,000 Hz; big 1,924,000,000 Hz | Samsung DTS cpufreq table entry `0x1b5fd0`, big-core `policy-max` / `pm_qos-booting = <0x1d5ba0>` (kHz), lines 10778–10800 |

Important unresolved hardware details:

* **Timer delivery:** Samsung's timer node also sets `use-clocksource-only`
  and `use-physical-timer`. [Mu's SoC configuration][mu-soc] selects
  `USE_PHYSICAL_TIMER=1`, `HAS_CNTFRQ_EL0_EMPTY_ERRATA=1` and
  `PcdTimerFreqOverwrite|26000000`. The XNU path currently uses CNTV and
  INTID 27. Its delivery and the live `CNTFRQ_EL0` value need measurement.
* **Display refresh:** [Mu `PlatformSec.c`][mu-sec] programs DECON
  `HW_SW_TRIG_CONTROL = 0x1281`. Successful writes to framebuffer RAM alone
  do not establish physical panel refresh. The active window address,
  stride, format, IOMMU state and trigger mode need capture from the S9.
* **RAM ownership:** the upper-bank reference is a candidate allocation,
  not a measurement of this phone's usable RAM. Fixed reservations and
  firmware/dynamic allocations must be accounted for. In particular,
  Samsung's DTS reserves `0xf0000000 + 0x09200000` for `modem_if`; the old
  manually chosen payload address `0xf0000000` conflicts with that region.
* **CPU metadata:** frequency values are table/policy metadata, not measured
  clocks. The ADT currently uses one SMP scheduler cluster and synthetic
  cache IDs; it does not implement Exynos power domains or DVFS.

## Implemented paths

* `pe_fiq.c` selects GICv2 or GICv3 from the ADT. GICv2 discovers per-CPU
  target masks, initializes banked timer/SGI state, handles UP target-mask
  read-as-zero, and preserves the full SGI source token through EOI.
  `sleh.c` and `SweetCPU.cpp` call the common acknowledge/EOI/IPI helpers.
* PSCI CPU_ON selects SMC/HVC from `/psci/method`; an absent node keeps the
  existing SMC default. CPU validation accepts MPIDR Aff1:Aff0. The platform
  and CPU IOKit personalities include `samsung,starlte` and
  `bergamota,arm-cpu`.
* Supplied hardware ADTs and `SweetCPU::startAll()` require boot MPIDR 0 and
  logical CPU 0. XNU defaults to one CPU; `cpus=N` selects an initial prefix
  of the ADT. Exynos secondary power-up remains to be tested.
* XNU reset and early-console mappings derive the framebuffer from
  `boot_args.Video`, including its final kernel-VA alias. The complete
  scanout must fit one 32 MiB block. EL2 secondary entry checks whether the
  optional GIC system-register interface exists before accessing it.
* The starlte ADT omits `/sweet-ufs`; the existing UFS-dependent peripheral
  startup returns before MMIO. SPMI initialization is explicitly gated on
  Qualcomm board identities. Framebuffer acceleration/backlight paths
  already require beryllium.
* Enxerto selects `starlte.c` plus common framebuffer/FDT/MMU/loader code.
  FDT diagnostics honor each parent's address/size cells. Initrd payloads
  are validated before segment writes, and exact-byte copy/BSS clearing
  handles odd-sized compressed segments.

### QEMU description

`--board starlte --qemu` selects QEMU `virt` hardware: GICD `0x08000000`,
GICC `0x08010000` (v2) or GICR `0x080a0000` (v3), 62.5 MHz timer, INTID 27,
PSCI HVC and contiguous physical MPIDRs. Default kernel RAM is
`0x50000000 + 0x60000000`, disjoint from the framebuffer at `0xcc000000`.
`--gic-version 2|3` and `--qemu-cpus 1..8` configure this description.
The runner checks that its ADT agrees with QEMU's MMIO and CPU layout.

## Build artifacts and Samsung handoff

From the repository root, generate ADTs for inspection and build the raw loader:

```sh
mkdir -p research/starlte
python3 tools/mkadt.py research/starlte/starlte.adt --board starlte --identity n104ap --dump
python3 tools/mkadt.py research/starlte/qemu.adt --board starlte --identity n104ap \
    --qemu --gic-version 2 --qemu-cpus 8
BOARD=starlte enxerto/build.sh
```

The identity option reads `research/ios26/n104ap.adt`. When that Apple
artifact is absent, generation reports it and omits `/product`; the local
component guests work with this reduced ADT.

Outputs are `enxerto/out/starlte/{Image,enxerto.elf}`. An optional absolute
payload path passed to `build.sh` is copied to `out/starlte/payload.bin`.
The raw ARM64 Image is position-independent and includes its BSS/stacks.
[`tools/mkboot-starlte.py`](../tools/mkboot-starlte.py) wraps it and a
SWEETXNU payload using the legacy Samsung layout described below. The
device's actual bootloader handoff remains to be tested.

The implemented entry contract is:

1. A Linux-style AArch64 handoff, with `x0` pointing to a readable physical
   FDT, translation and D-cache already off. Non-secure EL1 and bare EL2 are
   supported; the EL2 path drops to EL1. A resident hypervisor is outside
   this contract.
2. A 16 KiB-granule-capable CPU. Enxerto's temporary mapping has a 64 GiB
   address range; the image, payload and destination must fit within it.
3. `/chosen/linux,initrd-start` and `linux,initrd-end` locate an 8-byte-aligned
   raw SWEETXNU payload, outside the kernel RAM window. Both 32-bit and
   64-bit FDT property encodings are accepted. Placement must also keep
   the image, FDT, framebuffer and firmware reservations disjoint.
4. The inherited display already scans the framebuffer in `board.h`.
   Diagnostics print CPU registers and FDT memory/reservation data, but the
   loader still uses the configured board framebuffer and payload RAM;
   those diagnostics do not automatically select live addresses.
5. The final jump enters the payload at EL1, with MMU/I-cache/D-cache off,
   interrupts masked and `x0 = boot_args` physical address. The embedded
   Apple Device Tree is separate from the incoming Linux FDT.

Starlte defaults to XNU26's **1024-byte** boot_args CommandLine: structure
size 1152, `bootFlags` at 1136, `memSizeActual` at 1144. Existing Enxerto
boards default to the legacy **608-byte** ABI (size 736); select
`BOOT_ARGS_LINE_LENGTH=1024` for their XNU26 builds. SWEETXNU payloads
retain their 608-byte command-line field in both versions. The host QEMU
runner defaults to 1024, with `--boot-args-line-length 608` for old kernels.

### Samsung boot.img from a template

The user supplied `lineage-24.0-20261014-unofficial-starlte/boot.img`, SHA256
`ec07cbff03503baeb9ada4f72fe6166fbc67d209fd2fec584969336a4ecd5200`.
Observed layout: raw ARM64 Image, gzip Android ramdisk, separate **DTBH v2**
section, and the 16-byte `SEANDROIDENFORCE` footer. Its pre-versioned Android
header has **dt_size = 305152 at offset 40**, where modern tools expect
`header_version`. Page size is 2048; header load addresses are kernel
`0x10008000`, ramdisk `0x11000000`, tags `0x10000100`. These header fields
are not observations of the bootloader's actual RAM placement.

The DTBH has one Exynos9810 entry, hardware revisions 26–255, containing a
301372-byte FDT. Its `/chosen` contains placeholder initrd properties
`0x82000000` / `0x821fffff`; the bootloader must supply the actual payload
extent at handoff. This template has no `/memory` node and does not resolve
the live RAM/scanout questions above.

Once the full kernel is downloaded, build an early, single-core test image:

```sh
python3 tools/mkpayload.py xnu/out-starlte/kernel.development.sweet \
    research/starlte/starlte.adt research/starlte/payload.bin --compress \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress -enable_kprintf_spam'
BOARD=starlte BOOT_ARGS_LINE_LENGTH=1024 BOOTLOG=1 enxerto/build.sh
python3 tools/mkboot-starlte.py /path/to/lineage-starlte/boot.img \
    enxerto/out/starlte/Image research/starlte/payload.bin \
    research/starlte/boot-starlte-xnu26.img
```

The packager verifies the template's SHA-1 image ID and a byte-identical
roundtrip, replaces the kernel and ramdisk, and preserves the header's
other fields, second stage, DTBH and footer. The SHA-1 includes each of the
four sections followed by its little-endian size. Output files must be new.
The raw SWEETXNU payload replaces the Android ramdisk; this image contains
no iOS root filesystem. Packaging itself performs no device operations.

Observed format check: a repack using the real template, starlte Enxerto and
a synthetic component payload passes independent header/section/SHA checks.
The original template roundtrips byte-for-byte; five corruptions are
rejected. Evidence: `research/starlte/bootimg-check/summary.json`.

## Observed local results

Commands, prerequisites, exact test boundaries and output files:
[`tools/starlte-check/README.md`](../tools/starlte-check/README.md).
Tested tools: Clang 23.1.3 and QEMU 11.1.2, using `cortex-a76` for 16 KiB
translation support.

| Check | Result on 2026-10-29 |
| --- | --- |
| Interrupt guest (`check.py`) | PASS, GICv2/v3 × 1/8 CPUs; 32 timer IRQs per CPU, 4/200 SGIs per case; ELF/Mach-O compilation and host payload checks |
| Enxerto host (`enxerto.py`) | PASS, 12 cases: three boards × 608/1024-byte ABI × payload v1/v2; real producer, loader, FDT parser and inflater |
| Enxerto Image (`enxerto.py --qemu`) | PASS, four cases: GICv2/v3 × EL1/EL2; byte-exact synthetic kernel/ADT/boot_args and outgoing MMU/cache state |
| XNU reset (`reset.py`) | PASS, 12 cases: five cold resets, four eight-CPU warm resets, three final-framebuffer aliases; no unexpected exceptions |
| Real headers (`headers.py`) | Five successful complete translation-unit compiles, including production `genassym.c`, `arm_vm_init.c`, `pe_fiq.c`, `sweet_early.c`, `KernelConfigTables.cpp` |
| Extended C++ (`headers.py --cpp`) | Exits 1: 34 real MIG headers generated, then `SweetCPU.cpp` needs IIG-generated `DriverKit/OSObject.h`; `SweetPlatformExpert.cpp` encounters Apple-only `__builtin_xnu_type_*` in IOLib |
| Starlte Enxerto build | PASS, 655360-byte Image; header size/magic verified, no ABS64 relocations in C objects |
| Existing-board regressions | sweet/beryllium: 798720-byte Images, boot headers v2/v1, kernel/DTB roundtrips, 511 fastboot checks each; packaging used QEMU DTBs |
| ADT regression | Four sweet/beryllium hardware fixtures byte-identical to baseline; four default QEMU fixtures differ only by added PSCI HVC nodes |
| Ordered patch reconstruction | PASS: four staging scripts, 274 applied patches, two documented dxb skips; all 6369 source files and 14 pre-1131 baseline hashes match |

Patch 1131 is 25669 bytes across 12 files, SHA256
`cac36bd98fc5ed6c1c6cab7de24bddc069fc62cf16f705d073a9920374c890bf`.
The reconstruction uses XNU `xnu-12377.121.6` at
`ac9718fb1af618d5ce8678d0dc6e8a58f252216f` and dxb at
`663906528d1cd5ea8fde7de6f16de73db9511549`.
The freestanding guests establish component behavior; full XNU/IOKit
startup, Samsung firmware and physical device behavior remain untested.

## Next milestones

1. **Full XNU26 build.** GitHub CLI is authenticated as `friendlycatt`, and
   the S9 changes are pushed to the user's `friendlycatt/Bergamota` fork.
   [Run 40826795487](https://github.com/friendlycatt/Bergamota/actions/runs/40826795487)
   builds commit `46ccb6552bb5da6115af4df3c737090e3a7ecbc9`; its outcome is
   pending. Explicitly dispatch 26.5: `develop` otherwise defaults to 15.3.

   ```sh
   gh workflow run xnu.yml -R friendlycatt/Bergamota --ref develop \
       -f macos_version=26.5 -f machine_config=SWEET -f kernel_config=DEVELOPMENT
   gh run list -R friendlycatt/Bergamota --workflow xnu.yml --branch develop --limit 5
   gh run view RUN_ID -R friendlycatt/Bergamota --log
   gh run download RUN_ID -R friendlycatt/Bergamota -n xnu-kernel -D xnu/out-starlte
   ```

   Record the actual run ID, logs and artifact hash in `NOTES.md`; the
   earlier successful run `40412147688` predates S9 support. The local
   staging SDK contains only `TargetConditionals.h`, so it cannot replace
   the macOS/Apple build environment.
2. **Live handoff facts.** Identify the S9 model/SoC and bootloader state;
   obtain its running FDT, RAM/reservations, actual image placement, incoming EL,
   timer behavior and active DECON configuration. Keep dumps and device
   identifiers under ignored `research/`, `dtb/` or `backup/` paths.
3. **Single-core device progress.** Establish temporary loading, visible
   diagnostics and timer interrupts, then validate CPU_ON/IPIs on the real
   affinities. Device writes require the confirmations in `AGENTS.md`.
4. **Samsung peripherals and root boot.** Storage, display refresh, USB,
   input and power support are needed before an iOS userland boot can be
   claimed; Mali acceleration is a separate hardware-driver effort.

[samsung]: https://github.com/duhansysl/exynos9810-kernel/blob/914eabe7b6799ab0eb159360a5f9731dd50d0f1d/arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts
[mu]: https://github.com/Project-Silicium/Mu-Silicium/tree/7a2ada19841a129f86fb791c0b9a710436c849a4
[mu-dts]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Resources/DTBs/starlte.dts
[mu-memory]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Platforms/Samsung/starltePkg/Library/MemoryMapLib/MemoryMapLib.c
[mu-board]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Platforms/Samsung/starltePkg/starlte.dsc
[mu-soc]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Silicon/Samsung/S5E9810Pkg/S5E9810Pkg.dsc.inc
[mu-sec]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Silicon/Samsung/S5E9810Pkg/Library/PlatformSecLib/PlatformSec.c
