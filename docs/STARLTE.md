# Galaxy S9 / Exynos9810 bring-up

Target: Samsung Galaxy S9 **starlte** (Exynos9810, SM-G960F-class hardware),
using Bergamota's iOS 26 / XNU12377 stack. The Snapdragon S9 and S9+ have
different board descriptions.

**Status, 2026-10-29:** the full XNU26 CI build succeeded. The user's first
physical test remains on a **plain Samsung logo**, with no visible Enxerto
output. QEMU executes VM, CPU and IOKit initialization with readable verbose
diagnostics. An early display probe is in preparation; Samsung storage,
touch, Mali GPU, USB and power-management support remain follow-up work.

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
  `HW_SW_TRIG_CONTROL = 0x1281` at `0x16030070`. Enxerto now attempts this
  when the live FDT root identifies Exynos9810. Successful register or
  framebuffer RAM writes alone do not establish physical panel refresh.
  The active window address, stride, format, IOMMU state and trigger mode
  need capture from the S9.
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
* The loader preserves incoming EL/SCTLR across its optional EL2 drop and
  prints image/FDT/initrd addresses. A bounded root-compatible check selects
  the Samsung DECON write; QEMU's `linux,dummy-virt` takes the skip path.
  Readbacks include trigger state, DECON control/frame/TE counters, programmed
  and shadow window/channel maps, and six IDMA base/source/config/offset sets.
  `STARLTE_DISPLAY_PROBE=1` holds the early screen before payload loading.

### QEMU description

`--board starlte --qemu` selects QEMU `virt` hardware: GICD `0x08000000`,
GICC `0x08010000` (v2) or GICR `0x080a0000` (v3), 62.5 MHz timer, INTID 27,
PSCI HVC and contiguous physical MPIDRs. Default kernel RAM is
`0x50000000 + 0x60000000`, disjoint from the framebuffer at `0xcc000000`.
`--gic-version 2|3` and `--qemu-cpus 1..8` configure this description.
The runner checks that its ADT agrees with QEMU's MMIO and CPU layout.

## Build artifacts and Samsung handoff

### Ready test image

Local image: **`enxerto/out/starlte/boot-starlte-xnu26-verbose.img`**,
**9369616 bytes**. SHA256:

```text
94a0e8b2f1ac2f9790ec10816427bd988aac21ad926af52d81b95b406daa9d66
```

This contains the full CI kernel and starlte Enxerto with `BOOTLOG=1`,
single-core startup and `-v sweetearlyfb=1` diagnostics. It contains no iOS
root filesystem. Its actual loader, hardware payload, legacy image ID,
DTBH, footer and preserved header fields were independently verified.
The baseline image without the two verbosity options is retained as
`enxerto/out/starlte/boot-starlte-xnu26.img`.

The local delivery directory `research/starlte/delivery-40826795487/`
contains the image, kernel, loader, ADT, payload, provenance, QEMU screenshot
and a 12-file `SHA256SUMS`. The verified bundle is
`enxerto/out/starlte/starlte-xnu26-40826795487.tar.gz` (22660076 bytes).
Image and archive each have an adjacent `.sha256` file. These generated
artifacts are ignored by git.

### Reproduction

From the repository root, generate ADTs for inspection and build the raw loader:

```sh
mkdir -p research/starlte
python3 tools/mkadt.py research/starlte/starlte.adt --board starlte --identity n104ap --dump
python3 tools/mkadt.py research/starlte/qemu.adt --board starlte --identity n104ap \
    --qemu --gic-version 2 --qemu-cpus 1
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
4. The inherited display is configured to scan the framebuffer in `board.h`.
   On an Exynos9810 root FDT, Enxerto writes Mu-Silicium's trigger value to
   enable refresh. Diagnostics print CPU/display registers and FDT data;
   the loader still uses the configured framebuffer and payload RAM. Raw
   IDMA addresses may be IOVAs and do not automatically select RAM addresses.
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

Using the downloaded full kernel, build the verbose, single-core test image:

```sh
python3 tools/mkpayload.py xnu/out-starlte/kernel.development.sweet \
    research/starlte/starlte.adt research/starlte/payload-verbose.bin --compress \
    --cmdline '-v sweetearlyfb=1 debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress -enable_kprintf_spam'
BOARD=starlte BOOT_ARGS_LINE_LENGTH=1024 BOOTLOG=1 enxerto/build.sh
python3 tools/mkboot-starlte.py /path/to/lineage-starlte/boot.img \
    enxerto/out/starlte/Image research/starlte/payload-verbose.bin \
    enxerto/out/starlte/boot-starlte-xnu26-verbose.img
```

The packager verifies the template's SHA-1 image ID and a byte-identical
roundtrip, replaces the kernel and ramdisk, and preserves the header's
other fields, second stage, DTBH and footer. The SHA-1 includes each of the
four sections followed by its little-endian size. Output files must be new.
The raw SWEETXNU payload replaces the Android ramdisk; this image contains
no iOS root filesystem. Packaging itself performs no device operations.

Observed format checks pass for both the original synthetic fixture and
the delivered full-kernel image. The template roundtrips byte-for-byte;
five corruptions are rejected. Evidence:
`research/starlte/bootimg-check/summary.json` and delivery
`bootimg-verbose-verification.json`. The final payload differs from the
host-loader-validated baseline only in its 608-byte command-line field.

## Observed local results

Commands, prerequisites, exact test boundaries and output files:
[`tools/starlte-check/README.md`](../tools/starlte-check/README.md).
Tested tools: Clang 23.1.3 and QEMU 11.1.2, using `cortex-a76` for 16 KiB
translation support.

| Check | Result on 2026-10-29 |
| --- | --- |
| Interrupt guest (`check.py`) | PASS, GICv2/v3 × 1/8 CPUs; 32 timer IRQs per CPU, 4/200 SGIs per case; ELF/Mach-O compilation and host payload checks |
| Enxerto host (`enxerto.py`) | PASS, 12 cases: three boards × 608/1024-byte ABI × payload v1/v2; real producer, loader, FDT parser and inflater |
| FDT hardware gate (`enxerto.py`) | PASS, 24 exact root-list/malformed input cases; child identities do not select MMIO |
| Enxerto Image (`enxerto.py --qemu`) | PASS, four cases: GICv2/v3 × EL1/EL2; byte-exact synthetic kernel/ADT/boot_args and outgoing MMU/cache state |
| Display probe Image (`enxerto.py --qemu`) | PASS, EL1/EL2; held RGB strip/console, incoming EL preserved, dirty destination untouched; Samsung MMIO skipped |
| XNU reset (`reset.py`) | PASS, 12 cases: five cold resets, four eight-CPU warm resets, three final-framebuffer aliases; no unexpected exceptions |
| Real headers (`headers.py`) | Five successful complete translation-unit compiles, including production `genassym.c`, `arm_vm_init.c`, `pe_fiq.c`, `sweet_early.c`, `KernelConfigTables.cpp` |
| Extended C++ (`headers.py --cpp`) | Exits 1: 34 real MIG headers generated, then `SweetCPU.cpp` needs IIG-generated `DriverKit/OSObject.h`; `SweetPlatformExpert.cpp` encounters Apple-only `__builtin_xnu_type_*` in IOLib |
| Starlte Enxerto build | PASS, 655360-byte Image; header size/magic verified, no ABS64 relocations in C objects |
| Existing-board regressions | sweet/beryllium: 798720-byte Images, boot headers v2/v1, kernel/DTB roundtrips, 511 fastboot checks each; packaging used QEMU DTBs |
| ADT regression | Four sweet/beryllium hardware fixtures byte-identical to baseline; four default QEMU fixtures differ only by added PSCI HVC nodes |
| Ordered patch reconstruction | PASS: four staging scripts, 274 applied patches, two documented dxb skips; all 6369 source files and 14 pre-1131 baseline hashes match |
| Full XNU26 CI build | PASS, run 40826795487; actual ARM64 Mach-O and patch 1131 markers verified |
| Full hardware payload host loading | PASS, production C loader/FDT/inflater; 43761664 loaded bytes match the Python reference, including dirty BSS/boot_args destinations |
| Final Samsung image | PASS, exact loader/payload sections, preserved template metadata/DTBH/footer and independently verified SHA-1/SHA256 |
| Full-kernel QEMU | VM/CPU/IOKit initialization observed, 1/1 CPU and SGI1 verified; readable verbose screenshot; framebuffer-only decoder exits 3 (details below) |

Patch 1131 is 25669 bytes across 12 files, SHA256
`cac36bd98fc5ed6c1c6cab7de24bddc069fc62cf16f705d073a9920374c890bf`.
The reconstruction uses XNU `xnu-12377.121.6` at
`ac9718fb1af618d5ce8678d0dc6e8a58f252216f` and dxb at
`663906528d1cd5ea8fde7de6f16de73db9511549`.
The freestanding guests establish component behavior. The full-kernel
observations below extend this to QEMU startup execution. The physical
attempt has no evidence of Enxerto/XNU entry; an iOS root/userland boot
has not been established.

## Full-kernel build and QEMU observations

[Run 40826795487](https://github.com/friendlycatt/Bergamota/actions/runs/40826795487)
successfully built commit `46ccb6552bb5da6115af4df3c737090e3a7ecbc9`, completing
at `2026-10-29T06:26:05Z` with Xcode 26.1.1 (`17B100`). The downloaded kernel
is 22196640 bytes, SHA256
`9c09c6bd2aada6c608d5a67427961c16ed3e46c4e896c83364b8790c0ea9ea28`.
CI applied patch 1131; the kernel contains the starlte/CPU personalities and
GIC/PSCI helpers. `errors.txt` is empty. The local C++ infrastructure gaps
listed above did not prevent this complete macOS build.

For subsequent builds, explicitly dispatch 26.5; `develop` otherwise defaults
to 15.3. Use a fresh download directory for each run:

```sh
gh workflow run xnu.yml -R friendlycatt/Bergamota --ref develop \
    -f macos_version=26.5 -f machine_config=SWEET -f kernel_config=DEVELOPMENT
gh run list -R friendlycatt/Bergamota --workflow xnu.yml --branch develop --limit 5
gh run view RUN_ID -R friendlycatt/Bergamota --log
gh run download RUN_ID -R friendlycatt/Bergamota -n xnu-kernel -D xnu/out-starlte-RUN_ID
```

The actual kernel ran on QEMU 11.1.2 `virt`, cortex-a76, GICv2 and one CPU:

* **Baseline, 20 seconds:** framebuffer decoder exits **3** with
  `incomplete_decode`. The screen is black with a white bottom-left block.
  QEMU records 1119 IRQs and no other exceptions; PC/LR resolve to `cpu_idle`.
* **Baseline log capture, 20 seconds:** another exit 3; a 5178-byte RAM log
  records VM/BSD initialization, platform expert registration,
  `SWEET CPU: 1/1 CPUs initialized, SGI1 verified` and AppleCLCD creation.
  `rootvnode=0`; a root wait or root/userland boot is not established.
* **Verbose, 20 seconds:** adding `-v sweetearlyfb=1` produces a readable
  themed console screenshot and the same startup milestones in the RAM log.
  QEMU records 1573 IRQs, no other exceptions, and a `cpu_idle` snapshot.
  No panic appears in the captured ring. The framebuffer decoder still
  exits **3**, reporting `blank_screen`: it matches only all-white 2x
  `iso_font` glyphs, which this colored video console does not use.

The framebuffer-serial initialization gap is also observed in guest memory:
`fbcon_base` and geometry are set, but rows/cols are zero and `uart_initted`
is false. `serial_init()` returns before common initialization when the
generated ADT has no `/defaults/serial-device`. `PE_kputc` consequently uses
`console_write_unbuffered`. Verbose boot exposes that video console; it does
not repair framebuffer-serial initialization or the decoder.

Evidence is under `research/starlte/full-kernel-40826795487-{gic2,gic2-log,verbose}/`;
the delivery directory includes `qemu-verbose.png`, `qemu-verbose-log.txt`
and `qemu-verbose-summary.json`. These runs establish kernel initialization
progress, not a passing automated screen check or physical Samsung boot.

## First physical attempt

The user reports installing `boot-starlte-xnu26-verbose.img` through
**TWRP / OrangeFox** on an **SM-G960F / G960FD**. The screen stays on a
**plain Samsung logo, nothing else**. The selected partition was not
explicitly reported. No agent device writes were performed.

There is no observation yet that distinguishes failure to enter Enxerto,
an early loader stop, or framebuffer writes that never refresh the panel.
The delivered loader lacks Mu-Silicium's DECON trigger setup; that is a
concrete difference to probe, not a confirmed diagnosis.

Enxerto logs to the framebuffer and XNU has a volatile RAM ring. There is
no persistent S9 log file in this image; recovery `last_kmsg` / `pstore`
cannot reliably recover this attempt's output. The phone is not currently
in recovery, and the user asked to proceed without further recovery checks.

### Display probe 1

Build a probe around the existing verbose hardware payload:

```sh
BOARD=starlte BOOT_ARGS_LINE_LENGTH=1024 BOOTLOG=1 STARLTE_DISPLAY_PROBE=1 enxerto/build.sh
python3 tools/mkboot-starlte.py /path/to/lineage-starlte/boot.img \
    enxerto/out/starlte/Image research/starlte/delivery-40826795487/payload-verbose.bin \
    enxerto/out/starlte/boot-starlte-display-probe-1.img
```

It first paints a cyan strip, checks the incoming FDT identity, and applies
the pinned trigger write on Exynos9810. The completed screen is paper-coloured,
titled **ENXERTO / DISPLAY PROBE 1**, with a red/green/blue strip at the top.
It shows entry/register/FDT diagnostics and **holds before loading XNU**.
`PROBE READY` marks completion of those diagnostics. The initrd is included
to preserve the normal packaging and measure its handoff extent.

Local normal/probe builds and the extended loader suite pass. The raw probe
Image is 655360 bytes, SHA256
`17a66f71e430337af9058b48d095773425ed557780f6e34d6689080f899f1073`.
`research/starlte/display-probe-1-check/` contains 12 host-load results,
24 root-compatible cases, four full-loader handoffs and two probe holds.
The QEMU probe screenshots show the held diagnostics and RGB strip, with
the destination sentinel intact. Samsung display MMIO is skipped there;
the actual register write/readback and physical refresh remain untested.

The header's `EL in/now`, `SCTLRin`, `IMAGE`, `DTB`, `INITRD` and `RD END`
record loader entry and placement. `TRIGGER old/new`, `FRAMES old/now`,
`WINDOWS`, `CHANNELS` and `IDMA0..5` record display state. Counter samples
have no fixed time interval; equal samples do not establish a stalled panel.
The only display-register write is `0x1281` to `0x16030070`; IDMA bases,
formats, window selection and IOMMU state are reported rather than changed.

On the next user-controlled recovery test, the image target is **Boot**.
Photograph the held diagnostics if they appear. A continued plain Samsung
logo still does not distinguish loader entry from an unrefreshed/wrong
framebuffer; the next step is actual placement/display evidence and an
installed-partition check when recovery is available.

## Next milestones

1. **Early display probe.** Apply the pinned DECON trigger setup and hold
   visible entry/register diagnostics before XNU loading. Obtain the user's
   result; a continued logo leaves entry and scanout unresolved.
2. **Live handoff facts.** Obtain the bootloader state, running FDT,
   RAM/reservations, actual image placement, incoming EL,
   timer behavior and active DECON configuration. Keep dumps and device
   identifiers under ignored `research/`, `dtb/` or `backup/` paths.
3. **Single-core device progress.** Test the verbose image and capture the
   last visible screen. Establish temporary loading, visible
   diagnostics and timer interrupts, then validate CPU_ON/IPIs on the real
   affinities. Device writes require the confirmations in `AGENTS.md`.
4. **Console follow-up.** Complete framebuffer-serial initialization when
   it is the only registered interface; teach QEMU diagnostics to account
   for the themed video console or consume the captured log ring.
5. **Samsung peripherals and root boot.** Storage, display refresh, USB,
   input and power support are needed before an iOS userland boot can be
   claimed; Mali acceleration is a separate hardware-driver effort.

[samsung]: https://github.com/duhansysl/exynos9810-kernel/blob/914eabe7b6799ab0eb159360a5f9731dd50d0f1d/arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts
[mu]: https://github.com/Project-Silicium/Mu-Silicium/tree/7a2ada19841a129f86fb791c0b9a710436c849a4
[mu-dts]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Resources/DTBs/starlte.dts
[mu-memory]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Platforms/Samsung/starltePkg/Library/MemoryMapLib/MemoryMapLib.c
[mu-board]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Platforms/Samsung/starltePkg/starlte.dsc
[mu-soc]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Silicon/Samsung/S5E9810Pkg/S5E9810Pkg.dsc.inc
[mu-sec]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Silicon/Samsung/S5E9810Pkg/Library/PlatformSecLib/PlatformSec.c
