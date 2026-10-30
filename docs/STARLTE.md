# Galaxy S9 / Exynos9810 bring-up

Target: Samsung Galaxy S9 **starlte** (Exynos9810, SM-G960F / G960FD),
using Bergamota's iOS 26 / XNU12377 stack. The Snapdragon S9 and S9+ have
different board descriptions.

**Status, 2026-10-30:** the [probe-2 photograph](#photographed-ufs-probe-2-result)
shows the bounded **HCE enable succeeded** (`HCE=1`, `HCS=0x10f`, `IS=0`) and
the **FMP format call returned 0**. The first TEST UNIT READY command then
**timed out with `IS=0x4` (UIC_ERROR)**: **FAILED (60), zero writes / PROBE HOLD**.
The [probe-3 pre-cleanup diagnostics](#probe-3-pre-cleanup-diagnostics) and fresh
delivery/guard scripts are validated; Intel CI **41014066730** succeeded on
**4d12ded**. Fresh artifact reconciliation and image audits remain pending.
**Samsung media reads and an iOS root mount remain unverified**.
The [probe-2 delivery](#ufs-probe-2-software-delivery) and Intel XNU26 CI
**40955206538** are verified; all **148 manifest entries / 149 archive files**
match. Local transport/compilation checks and the audited QEMU failure hold pass.
The physical
[display probe](#display-probe-1) reached **PROBE READY**, and the full XNU26
CI build and QEMU VM/CPU/IOKit initialization are verified. Timer delivery,
touch, Mali GPU, USB and power-management support remain follow-up work.

The port reuses Heartwood, Renegade's arm64e/PAC rewriting, and the kernel PAC
fallback in patch 1019. The platform and storage work is in
[`1131-starlte-exynos9810-bringup.patch`](../xnu/patches-12377/1131-starlte-exynos9810-bringup.patch),
[`1132-starlte-readonly-ufs.patch`](../xnu/patches-12377/1132-starlte-readonly-ufs.patch),
[`1133-starlte-enable-inherited-ufs-host.patch`](../xnu/patches-12377/1133-starlte-enable-inherited-ufs-host.patch)
and [`1134-starlte-ufs-precleanup-diagnostics.patch`](../xnu/patches-12377/1134-starlte-ufs-precleanup-diagnostics.patch),
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

These are public reference-device facts used by
[`tools/mkadt.py`](../tools/mkadt.py) and [`enxerto/board.h`](../enxerto/board.h).
The physical probe now corroborates the selected framebuffer base/geometry
and the upper RAM bank advertised by the live FDT. Complete allocation
ownership and interrupt delivery still need device validation.

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

Hardware observations and remaining gaps:

* **Timer delivery:** Samsung's timer node also sets `use-clocksource-only`
  and `use-physical-timer`. [Mu's SoC configuration][mu-soc] selects
  `USE_PHYSICAL_TIMER=1`, `HAS_CNTFRQ_EL0_EMPTY_ERRATA=1` and
  `PcdTimerFreqOverwrite|26000000`. The XNU path currently uses CNTV and
  INTID 27. The photograph shows **`CNTFRQ_EL0=0`**; XNU's
  `pe_identify_machine()` already reads the ADT's **26 MHz**
  `timebase-frequency`. Actual virtual-timer interrupt delivery remains
  untested. The normal loader's optional CNTFRQ-based `BOOTPAUSE` is disabled.
* **Display refresh:** [Mu `PlatformSec.c`][mu-sec] programs DECON
  `HW_SW_TRIG_CONTROL = 0x1281` at `0x16030070`. Enxerto applies this when
  the live FDT root identifies Exynos9810. The physical probe shows readable
  output, trigger readback `0x3070 -> 0x1001` and frame count `2 -> 9`.
  IDMA0 reports base `0xcc000000` and 1440×2960 source geometry. Full
  stride/format/IOMMU interpretation and sustained XNU display operation
  remain to be validated; frame samples have no calibrated interval.
* **RAM ownership:** the live FDT advertises the selected
  `0x880000000 + 0x80000000` bank. Fixed reservations and firmware/dynamic
  allocations still must be accounted for. In particular,
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
* The hardware starlte ADT supplies Samsung UFS registers and zero DMA/Image
  placeholders. Enxerto fills them from its loaded Image; XNU validates the
  handoff before touching the host and performs read-only LUN/GPT discovery.
  UFS-dependent Qualcomm peripheral startup is gated explicitly, as is SPMI
  initialization. Framebuffer acceleration/backlight paths require beryllium.
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
* The padded starlte Image reserves a DMA32 arena after its stacks. Payload
  and destination/Image overlap is rejected before segment copies. The bounded
  ADT editor changes only the copied tree; XNU maps the pool Normal non-cacheable.

### QEMU description

`--board starlte --qemu` selects QEMU `virt` hardware: GICD `0x08000000`,
GICC `0x08010000` (v2) or GICR `0x080a0000` (v3), 62.5 MHz timer, INTID 27,
PSCI HVC and contiguous physical MPIDRs. Default kernel RAM is
`0x50000000 + 0x60000000`, disjoint from the framebuffer at `0xcc000000`.
`--gic-version 2|3` and `--qemu-cpus 1..8` configure this description.
The runner checks that its ADT agrees with QEMU's MMIO and CPU layout.
QEMU ADTs omit UFS by default; QEMU `virt` does not model the Samsung host.

## Build artifacts and Samsung handoff

### XNU-enabled refresh image

**Earlier XNU-enabled test image:** `enxerto/out/starlte/boot-starlte-xnu26-refresh.img`,
**9369616 bytes**, with adjacent `.img.sha256`. SHA256:

```text
f9c6df75c666d11d0e5ff06a29c581c7092ff5204d0183d55803406f3ca398d6
```

Built with `BOARD=starlte BOOT_ARGS_LINE_LENGTH=1024 BOOTLOG=1` and
`STARLTE_DISPLAY_PROBE=0`, this enables normal XNU loading and retains the
DECON setup that produced the successful physical probe. The loader source
is `e4d521886d338a953c6c3476d77702979e11451d`; its Image and ELF are
byte-identical to the four passing normal GICv2/v3 × EL1/EL2 loader handoffs.
The 655360-byte Image has SHA256
`2831c1de7055c858684ccd5cb9a664cda8fb78362d2d254f14f8cb2140777fa8`.

It uses the existing kernel from CI run **40826795487** and the unchanged
8406896-byte verbose hardware payload, SHA256
`6d5ce738b6562ced9fa93452b070cb53c97ef5fc5cee554f3a626e8d590a70e4`.
Arguments remain:

```text
-v sweetearlyfb=1 debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress -enable_kprintf_spam
```

Independent verification confirms the exact loader/payload sections,
preserved template metadata/second stage/DTBH/footer and legacy SHA-1.
`research/starlte/xnu26-refresh-delivery/` contains the image, loader,
verification, provenance, photograph observations, README and ten-file
`SHA256SUMS`. These artifacts are local and ignored by git.

After delivery, the user reports visible XNU output ending in the NVRAM
message described [below](#reported-xnu-root-device-wait). The exact installed
bytes and full surrounding screen have not been supplied. The image includes
no iOS root filesystem.

### Initial full-kernel image

The user tested this image and reported the plain Samsung logo. It is
preserved alongside the current [refresh image](#xnu-enabled-refresh-image).

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
The raw ARM64 Image is position-independent and includes its BSS/stacks and
starlte DMA arena (currently **1740800 bytes** in the normal logging build).
[`tools/mkboot-starlte.py`](../tools/mkboot-starlte.py) wraps it and a
SWEETXNU payload using the legacy Samsung layout described below. The
physical probe established loader entry and live placement; the later user
report supports XNU/BSD execution on the S9.

The implemented entry contract is:

1. A Linux-style AArch64 handoff, with `x0` pointing to a readable physical
   FDT and translation off. Non-secure EL1 and bare EL2 are
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

The probe photograph shows incoming/current `SCTLR=0x30c5183e`: M is clear,
while C and I are set. The earlier assumption of cache-enable bits being
clear does not match this observation. `mmu_enable_ranges()` invalidates
the image, payload and destination ranges plus the instruction cache before
enabling its mappings. The physical probe holds before that mapping/loading
path; the later refresh-image report supports progress through it into XNU.

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
extent at handoff. This template has no `/memory` node. The later physical
probe reports live initrd `0x89000000..0x89804770` and the selected upper
RAM bank, showing that the bootloader supplies additional live FDT data.

Using the downloaded full kernel, rebuild a verbose single-core image with
the current loader (the original delivered image above is preserved):

```sh
python3 tools/mkpayload.py xnu/out-starlte/kernel.development.sweet \
    research/starlte/starlte.adt research/starlte/payload-verbose.bin --compress \
    --cmdline '-v sweetearlyfb=1 debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress -enable_kprintf_spam'
BOARD=starlte BOOT_ARGS_LINE_LENGTH=1024 BOOTLOG=1 STARLTE_DISPLAY_PROBE=0 enxerto/build.sh
python3 tools/mkboot-starlte.py /path/to/lineage-starlte/boot.img \
    enxerto/out/starlte/Image research/starlte/payload-verbose.bin \
    enxerto/out/starlte/boot-starlte-xnu26-rebuild.img
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

| Check | Observed result |
| --- | --- |
| Interrupt guest (`check.py`) | PASS, GICv2/v3 × 1/8 CPUs; 32 timer IRQs per CPU, 4/200 SGIs per case; ELF/Mach-O compilation and host payload checks |
| Enxerto host (`enxerto.py`) | PASS, 12 cases: three boards × 608/1024-byte ABI × payload v1/v2; real producer, loader, FDT parser and inflater |
| FDT hardware gate (`enxerto.py`) | PASS, 24 exact root-list/malformed input cases; child identities do not select MMIO |
| Enxerto Image (`enxerto.py --qemu`) | PASS, four cases: GICv2/v3 × EL1/EL2; byte-exact synthetic kernel/ADT/boot_args and outgoing MMU/cache state, including Image-owned DMA export |
| Display probe Image (`enxerto.py --qemu`) | PASS, EL1/EL2; held RGB strip/console, incoming EL preserved, dirty destination untouched; Samsung MMIO skipped |
| XNU reset (`reset.py`) | PASS, 12 cases: five cold resets, four eight-CPU warm resets, three final-framebuffer aliases; no unexpected exceptions |
| Real headers (`headers.py`) | Five successful complete translation-unit compiles, including production `genassym.c`, `arm_vm_init.c`, `pe_fiq.c`, `sweet_early.c`, `KernelConfigTables.cpp` |
| Extended C++ (`headers.py --cpp`) | Exits 1: 34 real MIG headers generated, then `SweetCPU.cpp` needs IIG-generated `DriverKit/OSObject.h`; `SweetPlatformExpert.cpp` encounters Apple-only `__builtin_xnu_type_*` in IOLib |
| Starlte Enxerto build | PASS, 1740800-byte normal Image including DMA arena; header size/magic verified, no ABS64 relocations in C objects |
| Existing-board regressions | sweet/beryllium: 798720-byte Images, boot headers v2/v1, kernel/DTB roundtrips, 511 fastboot checks each; packaging used QEMU DTBs |
| ADT regression | Four sweet/beryllium hardware fixtures byte-identical to baseline; four default QEMU fixtures differ only by added PSCI HVC nodes |
| UFS (`ufs.py`) | PASS, 161 byte-level host scenarios with fatal ASan/UBSan, 24 ADT ownership/bounds cases, complete real-header Mach-O compilation |
| Ordered patch reconstruction | PASS: local four-stage / 277-patch reconstruction, 6369 files and baseline hashes exact; prior probe-2 CI profile (276 patches) reconstructs 6374 files with six dxb substitutions and real KDK staging; two documented dxb skips |
| Full XNU26 CI build | PASS, run 40955206538; actual ARM64 Mach-O, checksum, HCE markers and patches 1131–1133 verified |
| Full hardware payload host loading | PASS, production C loader/FDT/inflater; 43778048 loaded bytes match the Python reference, including dirty BSS/boot_args destinations |
| Final Samsung image | PASS, exact loader/payload sections, preserved template metadata/DTBH/footer and independently verified SHA-1/SHA256 |
| Full-kernel QEMU | Probe-2 checker 0 after 90 seconds: expected missing-node FAILED/PROBE HOLD, 7078 console bytes, 7055 IRQs and one verified retried Skywalk fault; framebuffer-only decoder exits 3; Samsung UFS not modeled |
| Renegade platform/root preview | PASS, 24 tests and one macOS-only signer-parity skip; includes ios18/ios26 × Linux/macOS full previews before downloads |

Patch 1131 is 25669 bytes across 12 files, SHA256
`cac36bd98fc5ed6c1c6cab7de24bddc069fc62cf16f705d073a9920374c890bf`.
Patch 1132 is 34532 bytes for `bsd/dev/sweet_ufs.c`, SHA256
`6ee211f1f329c2f572dde7cc614780227da45b11e21bdaf9dfed9299164587a6`.
Patch 1133 is 6172 bytes, SHA256
`aeb561be8a745543a00dd7216140b45ea49e90bb30a10231c0923709c809c73b`.
The reconstruction uses XNU `xnu-12377.121.6` at
`ac9718fb1af618d5ce8678d0dc6e8a58f252216f` and dxb at
`663906528d1cd5ea8fde7de6f16de73db9511549`. The latest local reconstruction
is retained in ignored `research/starlte/series-check-ufs-probe-2/`; the reviewed
CI-profile replay is in
`research/starlte/ufs-probe-2-bringup/ci-40955206538-source-profile-v2/`.
The freestanding guests establish component behavior. The full-kernel
observations below extend this to QEMU startup execution. The physical
probe establishes Enxerto entry and visible panel refresh. The later user
report supports physical XNU/BSD execution; an iOS root/userland boot has
not been established.

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

That image's logo alone did not distinguish failure to enter Enxerto,
an early loader stop, or framebuffer writes that never refresh the panel.
It lacked Mu-Silicium's DECON trigger setup. The later probe below adds
that setup and establishes readable physical output; the exact cause of
the original attempt's lack of output was not isolated.

Enxerto logs to the framebuffer and XNU has a volatile RAM ring. There is
no persistent S9 log file in this image; recovery `last_kmsg` / `pstore`
cannot reliably recover this attempt's output. The user asked to proceed
without further recovery checks.

### Display probe 1

**Tested probe image:** `enxerto/out/starlte/boot-starlte-display-probe-1.img`,
**9369616 bytes**, with adjacent `.img.sha256`. SHA256:

```text
8918981c26ddc8402f194298b2a68f963f0f999c504cae1973857db3c92f4195
```

Loader source: `e4d521886d338a953c6c3476d77702979e11451d`. It reuses the
verbose hardware payload and kernel from CI run **40826795487**. The
user's photograph confirms **PROBE READY**, readable text and the RGB strip.
`XNU loading disabled` is the intentional pre-XNU hold.

Build a probe around the existing verbose hardware payload:

```sh
BOARD=starlte BOOT_ARGS_LINE_LENGTH=1024 BOOTLOG=1 STARLTE_DISPLAY_PROBE=1 enxerto/build.sh
python3 tools/mkboot-starlte.py /path/to/lineage-starlte/boot.img \
    enxerto/out/starlte/Image research/starlte/delivery-40826795487/payload-verbose.bin \
    enxerto/out/starlte/boot-starlte-display-probe-1-rebuild.img
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
the destination sentinel intact. Samsung display MMIO is skipped in QEMU;
the physical photograph supplies the register/refresh observations below.

Independent packaging verification passes: the kernel section is exactly
the QEMU-checked probe Image, the payload is unchanged, and template header
metadata/second stage/DTBH/footer and legacy SHA-1 are verified. The actual
template's FDT also matches the production Exynos9810 identity gate.
Provenance, verification, check log, disassembly and screenshots are under
ignored `research/starlte/display-probe-1-delivery/`.

The header's `EL in/now`, `SCTLRin`, `IMAGE`, `DTB`, `INITRD` and `RD END`
record loader entry and placement. `TRIGGER old/new`, `FRAMES old/now`,
`WINDOWS`, `CHANNELS` and `IDMA0..5` record display state. Counter samples
have no fixed time interval; equal samples do not establish a stalled panel.
The only display-register write is `0x1281` to `0x16030070`; IDMA bases,
formats, window selection and IOMMU state are reported rather than changed.

**Physical observations, 2026-10-29:** manually transcribed from the user's
photograph, rather than a binary register/FDT dump:

| Field | Visible value |
| --- | --- |
| Entry/current EL | EL1 / EL1 |
| Incoming/current SCTLR | `0x30c5183e` (M clear, C/I set) |
| MPIDR | `0x81000000` |
| CNTFRQ_EL0 | `0` |
| Loader Image | `0x80098000..0x80138000` |
| FDT | `0x8a000000` |
| Initrd | `0x89000000..0x89804770` (8406896 bytes, exactly the verbose payload size) |
| DECON trigger readback | `0x3070 -> 0x1001` after the `0x1281` write |
| Frame count | `2 -> 9` |
| IDMA0 base/source size | `0xcc000000` / `0x0b9005a0` (1440×2960) |
| Selected RAM bank in live FDT | `0x880000000 + 0x80000000` |

Full manual transcription stays ignored in
`research/starlte/device-probe-1/observation.json`. The held probe establishes
loader/display progress. The subsequent
[XNU-enabled refresh build](#xnu-enabled-refresh-image) produced the user's
XNU report below; timer interrupt delivery and full RAM ownership remain open.

## Reported XNU root-device wait

After refresh delivery, the user says it now works and copies
`Failed to write boot picker bring up reason to nvram`. No new photograph
or complete log accompanied the report. In the prepared XNU tree:

* `iokit/bsddev/IOKitBSDInit.cpp:960` calls
  `IOSetRecoveryBoot(BSD_BOOTFAIL_MEDIA_MISSING, NULL, true)` after root-device
  matching times out.
* Lines 609–615 try to store `boot-picker-bringup-reason = missing-boot-media`
  and emit the matching NVRAM error. Lines 630–642 subsequently wait for
  `IODTNVRAM` / `IONVRAM`, which can explain why the screen stops there.

The report therefore supports physical XNU/BSD root discovery. The NVRAM
error is secondary to missing boot media. Fixing NVRAM alone would not supply
a root device.

An inspection of the delivered refresh image confirms that its payload has
no RAMDisk, `rd=` / `rootdev=` argument, `/chosen/root-matching` or boot UUID.
Its older starlte ADT omits `/sweet-ufs`. That image's
`bsd/dev/sweet_ufs.c:1036–1040` returns `ENXIO` without that node. Thus flashing
an iOS filesystem to userdata alone cannot make this kernel read it.

### Samsung storage prerequisites

The [pinned Samsung driver][samsung-ufs] and DTS identify:

| Item | Reference finding | Implemented handling |
| --- | --- | --- |
| Registers | HCI `0x11120000 + 0x200`, vendor HCI `0x11121100 + 0x200` | Exact ADT validation, mapped separately; inspect inherited HCE/HCS/request lists |
| Descriptor units | `UFSHCD_QUIRK_PRDT_BYTE_GRAN`, `ufs-exynos.c:559`; [ufshcd.c][samsung-ufshcd]:1695,3235 | Byte-based response/PRDT offsets and lengths |
| PRDT layout | `CONFIG_EXYNOS_FMP=y`; [ufshci.h][samsung-ufshci] and [fmp.c][samsung-fmp] | 128-byte entries, zero encryption selectors, DESCTYPE 3 via `SMC_CMD_FMP_SECURITY(0, 0, 3)` |
| PRDT data unit | `PRDT_SET_SIZE(12)` at `ufs-exynos.c:442–444` | TX/RX low five bits set to 12: log2(4096), **not** a 12-byte descriptor stride |
| Request quirks | Positive-bit `UTRLCLR`; vendor Nexus register offset `0x40` | Clear slot 0 with `1`; set and verify its Nexus bit before every command |
| DMA addressing | Initial `DMA_BIT_MASK(32)`; `ufshcd_set_dma_mask()` can select 64-bit addressing when CAP.64AS permits | Use an Image-owned DMA32 arena even though XNU manages upper RAM |

Probe 2 observes a zero return from the descriptor-format SMC. Actual data DMA,
descriptor acceptance and the failing link's condition remain unmeasured.
The implementation reuses inherited host/link configuration; it
does not initialize the Exynos PHY or recover a failed link.

### Read-only Samsung UFS

Patch 1132 extends `bsd/dev/sweet_ufs.c` with an Exynos-specific transport
selected by the `samsung,starlte` board identity:

1. **Owned DMA handoff.** Enxerto's `.ufs_dma` section reserves `0x108000`
   bytes after the stacks, within the padded Image. Runtime 16 KiB alignment
   yields a `0x104000`-byte pool: 16 KiB of descriptors and a 1 MiB bounce
   buffer. `enxerto/adt.c` fills `/sweet-ufs/dma-pool` and `loader-image` in
   the copied ADT. Both loader and kernel validate size, alignment, containment,
   DMA32 bounds and separation from XNU's managed RAM. The kernel uses
   `ml_io_map_wcomb()` (Normal non-cacheable) and explicit DMA barriers.
2. **Conservative host takeover.** Require inherited device/list/UIC readiness
   and idle transfer/task lists. Additive patch 1133 allows one bounded HCE
   enable-only attempt after stopping both lists; enabled hosts are not restarted.
   Require HCE and all readiness bits afterward, without host error or new
   pending requests, before the FMP format SMC
   (`x0=0xc2001810`, `x1=x2=0`, `x3=3`). Check PRDT register setup and the new
   list-base readback before issuing commands. An Exynos timeout permanently
   quarantines the arena, even when request-clear succeeds; cached initialization
   failures prevent a later root lookup from reusing it.
3. **Read-only discovery.** Scan normal LUNs 0–7 with TEST UNIT READY and
   READ CAPACITY(10/16), accepting 512–4096-byte power-of-two blocks. READ(10/16)
   uses bounded transfers. Primary GPT geometry, header/entry CRCs, partition
   bounds and case-insensitive ASCII names are checked. Read the first 4 KiB
   of `userdata` twice, poisoning the bounce buffer between reads; print a CRC
   and filesystem hint, without dumping partition contents.
4. **Read-only enforcement.** Exynos ignores `ufsrw=1`; the command boundary
   permits only the discovery/read opcodes. Write, flush, persistent-log and
   auxiliary Qualcomm-partition paths cannot issue Samsung writes. Qualcomm
   retains its existing writable-root features and descriptor format.

Probe payload arguments:

```text
-v sweetearlyfb=1 sweetfb_statlog=0 debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress -enable_kprintf_spam rd=ufsroot0 ufsprobe=1
```

Build the loader with `STARLTE_DISPLAY_PROBE=0`. Here **`ufsprobe=1` is a
kernel storage probe**, not the earlier loader-only display probe: it prints
`STARLTE UFS: probe PASSED` or `FAILED`, write/failure counters and
`PROBE HOLD`, then waits before BSD disk registration or root mounting.
Early ADT/DMA/mapping failures also reach the hold. Photograph the whole
screen, including the controller/capacity/GPT lines. If execution stalls
earlier, the last visible line identifies the next investigation point.
`sweetfb_statlog=0` disables the existing periodic framebuffer IOLogs so they
cannot scroll the held result off-screen; it does not disable display refresh.

**Original probe-1 local results:** `ufs.py --out research/starlte/ufs-check-final` exits 0:
**93 byte-level host scenarios**, **24 ADT ownership/bounds cases**, and
complete `sweet_ufs.c` real-header Mach-O compilation pass, with fatal
ASan/UBSan checking. `enxerto.py --qemu` also passes 12 host payload cases,
24 FDT cases, four normal handoffs (one verifies real DMA export), and two
display-probe holds. Shared sweet/beryllium builds pass their kernel/DTB
roundtrips and 511 host fastboot checks each. Exact service boundaries are
documented in [`tools/starlte-check/README.md`](../tools/starlte-check/README.md).
No Samsung storage hardware result or iOS root mount is established yet.

**Build provenance:** checked implementation
**`6584f5d1a6211fd8b09b43ebb7ac7d558457d6ea`**. CI run
[40916280553](https://github.com/friendlycatt/Bergamota/actions/runs/40916280553)
initially failed **before any build step**, because GitHub could not acquire
a macOS arm64 runner. The unchanged-revision retry was still unallocated
when the Intel fallback was dispatched. Intel-host run
[40931557218](https://github.com/friendlycatt/Bergamota/actions/runs/40931557218),
source **`209eb05ac5b2db3bfd82446ecf9c50b2759f2626`**, **succeeded** on
**x86_64 / Xcode 26.1.1 / 17B100**, completed **2026-10-29T19:23:56Z**.
Build took **48m 40s**; dispatch-to-completion **52m 51s**. **275 patches**
applied, including 1131/1132, with only the two documented dxb skips.
The downloaded `xnu-kernel` artifact is **12927160373**; CI checksums pass
and `errors.txt` is zero bytes. Separately inspected linker summaries contain
host-tool/linker warnings and interleaved warning fragments, not an established
build failure. The newer probe-1 photograph below reaches UFS initialization;
the refresh image above predates UFS.
The workflow now offers the standard Intel build-host fallback
[`runner_arch=intel`](CI_CACHE.md#macos-26-runner-capacity-fallback), without
changing the ARM64 kernel target or patch series.

**Late watcher follow-up, 2026-10-30T04:25Z:** the old arm64 retry watch ended
on a network timeout; a later status query also reported a connectivity error.
Fresh read-only `gh run view` succeeds: **40916280553** is still **queued**,
with **no current jobs or conclusion**, while probe-2 **40955206538** remains
**completed/success**. The network failure did not establish a kernel failure.
Original logs and the fresh queries are retained separately in
`research/starlte/ufs-probe-2-bringup/ci-40916280553-late-watcher-review/`.

### Read-only UFS probe 1 image

**Delivered and subsequently photographed on the phone:**
`enxerto/out/starlte/boot-starlte-ufs-probe-1.img`, **10459152 bytes**, with
adjacent `.img.sha256`. SHA256:

```text
827720a8383404df538d95513aef50d06624867ddbbc566064e6f1c03ca99002
```

The image packages the checked **1740800-byte** normal Enxerto Image as
Samsung's kernel section and the **8410064-byte** hardware SWEETXNU payload
as its ramdisk section. That payload contains the actual new ARM64
DEVELOPMENT/SWEET kernel, **22213416 bytes**, SHA256
`96e56ef2884607ffff6e1ba5de962bd77e0e77ce32c522884d4b78120bad5558`.
There is **no iOS root filesystem** in the Samsung ramdisk section.

Independent checks verify exact loader/payload sections, unchanged Samsung
template metadata/second/DTBH/footer and legacy SHA-1. The complete production
host-loader comparison passes **43778048 loaded RAM bytes**, with dirty
destinations and unchanged source payload; its host adapters do not execute
XNU. The delivery's Image/ELF match the earlier checked normal loader, and
all **61 recorded source hashes** still match. The kernel patch/staging series
and loader/producers are unchanged between the checked and CI commits.

**Full-kernel QEMU result:** fresh
`research/starlte/ufs-probe-1-qemu-40931557218-audited/`, **90 seconds**,
capture checker **0**. The **7078-byte** complete console ring and separately
viewed screenshot show exactly one:

```text
STARLTE UFS: probe FAILED (6), writes 0, commands failed 0
STARLTE UFS: PROBE HOLD - photograph this screen, then reboot to recovery
```

QEMU has **no Samsung UFS host**: this is the expected early missing-node
failure/hold, before storage MMIO, **not a storage pass**. `rootvnode=0`,
`early_panicking=0`; snapshot PC is `_cpu_idle + 0x220`. The trace contains
**7021 IRQs and one retried Skywalk demand-page fault**, not zero Data Aborts.
Its exact version-store instructions, runtime pageable-map first address,
EL1 write translation syndrome and same-PC retry are independently checked;
unexpected/repeated/non-retried faults remain fatal. The first IRQ-only
checker failure is preserved unchanged. An older pre-UFS kernel reproduces
the demand fault but later panics on the unavailable requested `ufsroot0`
because it lacks the new hold; that failed comparison is **not** a baseline
pass. The underlying framebuffer runner still exits **3 / blank_screen**
because its white-font decoder misses the readable coloured console.

**Bundle:** `enxerto/out/starlte/starlte-ufs-probe-1-40931557218.tar.gz`,
**29551691 bytes**, SHA256
`257afadde9d259cb4a68e7349338489179a58d768630fdd15864885fbdf96b23`,
with adjacent `.gz.sha256`. The ignored delivery directory
`research/starlte/ufs-probe-1-delivery/` contains the image, kernel, loader,
ADTs/payloads, README, CI/source provenance, checks and raw QEMU captures.
All **51 manifest entries / 52 archive files** independently verify. Original
CI preparation run **40916280553** is preserved separately from successful
run **40931557218**. Failed captures, detailed fault review and final audits
remain under ignored `research/starlte/`; the older refresh image's checksum
is unchanged. No device dumps or downloaded artifacts were added to git.

Probe tests use the already selected **TWRP / OrangeFox test-image boot route**.
Preserve recovery, bootloaders, modem, persist and partition backups;
**do not format or write userdata for this probe**. No assistant phone writes,
Samsung storage read, iOS root mount or userspace execution is established.

### Photographed UFS probe 1 result

The user's photograph, received on 2026-10-29 after probe-1 delivery, shows:

```text
STARLTE UFS: DMA32 0x801a0000+0x104000 inside Image 0x800fc000+0x1ac000, Normal-NC
SWEET UFS: host not ready (hce 0x0 hcs 0x10f)
STARLTE UFS: probe FAILED (6), writes 0, commands failed 0
STARLTE UFS: PROBE HOLD
```

Clearly transcribed host registers: **CAP `0x1303ff0f`**, **VER `0x210`**,
**IS/transfer DBR `0`**, **transfer RSR `1`**. The UTRL base is not reliably
transcribed; task-list registers were not captured. No installed-image checksum
was supplied. The new markers establish physical execution of this UFS path,
not the exact installed bytes.

The DMA pool ends at `0x802a4000`, within the Image mapping ending at
`0x802a8000`, leaving `0x4000`. This supports successful handoff validation and
mapping, **not actual controller DMA access**. `enxerto/mmu.c` rounds the Image
mapping outward to 16 KiB pages: the photographed `0x1ac000` mapping span is
not the 1740800-byte raw Image size. These are runtime observations, not
addresses to hardcode into another image.

Patch 1132 rejects HCE=0 at `ufs_take_over_host()` before FMP setup, list-base
replacement or storage commands. “Scanning LUNs 0..7” is an announcement;
there are no photographed LUN/capacity/GPT/sample results. The zero counters
are consistent with this early rejection. Full transcription and limits stay
ignored in `research/starlte/device-ufs-probe-1/observation.json`; this session
has checkpoint-carried visible values, not a local copy of the original JPEG.

**Source review:** [Samsung `ufshcd.c`][samsung-ufshcd]:983–985 returns zero
when HCE bit 0 is **set**. `ufs-exynos.c:551–568` does not assign
`UFSHCD_QUIRK_USE_OF_HCE`; merely finding its generic alternate path does not
justify an S9 HCE bypass. The normal enable sequence writes HCE=1, waits 1 ms,
then polls with at most ten 5 ms delays (4049–4101). HCS `0x10f` has DP,
transfer/task/UIC readiness set; its upper power-change status is `PWR_LOCAL`.
Samsung's operational gate requires all three list/UIC readiness bits
(804–815,4005–4015). The photograph alone does not establish why HCE is zero.

A bounded **enable-only** continuation is implemented in additive
[`1133-starlte-enable-inherited-ufs-host.patch`](../xnu/patches-12377/1133-starlte-enable-inherited-ufs-host.patch):
**6172 bytes**, SHA256
`aeb561be8a745543a00dd7216140b45ea49e90bb30a10231c0923709c809c73b`.
Patch 1132 and completed probe-1 artifacts remain unchanged.

Only an inherited ready/idle Exynos host can reach the attempt. Both lists
must stop first; HCE=1 is then written once, followed by a 1 ms wait and at
most ten 5 ms delays. Already-enabled hosts are not restarted. Device,
transfer/task/UIC readiness, error status and idle/stopped lists are checked
again before FMP/list-base setup or storage commands. The code adds read-only
task-list and vendor reset/clock/GPIO diagnostics; it does not write those
vendor registers, bypass HCE, calibrate the PHY or issue `DME_LINKSTARTUP`.
If enabling loses readiness, it reports that and stops. This is **not complete
cold-host initialization**, nor proof that enabling will preserve the real link.

**Correction validation:** `ufs.py --out research/starlte/ufs-probe-2-check`
exits **0**: **135 host scenarios**, **24 ADT ownership/bounds cases**, fatal
ASan/UBSan and complete real-header Mach-O compilation pass. The host models
the photographed HCE=0/HCS=0x10f state, immediate write echo, initialization
transition, exact wait bounds, readiness/error/list-state loss, late completion
and cached failure. Software MMIO allowlists forbid blind PHY/clock/reset or
UIC command writes; Qualcomm regressions remain covered. The new driver hash
is `5b73d355232fc4d9319f43245c3785bbe7301e43b2947617c4ca118200b892ef`.
Forward/reverse application matches the saved probe-1 baseline exactly.
Fresh ordered-series reconstruction also passes: four staging scripts,
**276 patches**, the two documented dxb skips, and all **6369 source files**
match the prepared tree exactly (`research/starlte/series-check-ufs-probe-2/`).
All 61 original probe-1 source hashes and its image/archive checksums remain
unchanged. The correction is committed/pushed as
**`f359612c182a30f27071210b7baa25986b1fe08f`**. Fresh Intel-host XNU26
[CI 40955206538](https://github.com/friendlycatt/Bergamota/actions/runs/40955206538)
is **SUCCESS**, completed **2026-10-29T21:53:15Z** (26.5 / DEVELOPMENT / SWEET;
**41m 12s Build / 44m 42s total**, x86_64 / Xcode **26.1.1 / 17B100**).
Both kernel/build-tree downloads completed; the strict source inspection failed
on **eight changed / five additional files**, with no missing files and the actual
CI UFS driver matching the tested source. The four build-file deltas match pinned
dxb substitutions; the remaining files reflect CI's real **KDK 26.5 / 25F71** V2
headers and **MacOSX26.1.sdk** TargetConditionals instead of local stubs/older SDK.
The original failure and every delta are preserved locally. A separately replayed
reviewed CI profile now passes: **6374/6374 exact source files**, **six pinned dxb
substitutions / five staging scripts / 276 patches**, no missing/different/extra
files or exclusions. All nine external header copies are individually byte/hash
pinned to the reviewed CI inputs; this does not claim an independent Apple
SDK/KDK reinstall or a byte-identical kernel rebuild. The initial replay's
incorrect full-build stage selection and its failed result also remain preserved.
Fresh inspection passes ARM64/checksum/HCE diagnostics/empty errors and the full
patch order: kernel **22213456 bytes**, SHA256
**`c4350d0f7d3d15312cdaadfb9c9f9a15f7bbd624085946d69b907d0466d45765`**.
The separate probe-2 image and fresh real-kernel QEMU check are complete as
recorded below. No download remains active and no new CI run was dispatched for
source reconciliation.

**Probe-2 preparation:** separate ignored `research/starlte/ufs-probe-2-delivery/`
now holds the unchanged checked Image/ELF and fresh hardware/QEMU ADTs.
The DMA/Image properties remain runtime-filled placeholders, not historic
phone addresses. All **351 loader/producer/check-tool/patch/staging/workflow
source hashes** match f359612; **158 protected probe-1 files** are unchanged.
Initial preparation passed nine reproduction-script syntax checks and **18
delivery-guard tests**; the reviewed CI-profile revision passes **ten syntax
checks / 23 unittest methods with adversarial subtests**. Fresh synthetic
finalizer/archive integration passes **eight negative guards / 118 fixture
manifest files / exact hashes**, explicitly mocking only the old binary's kernel
marker guard for format testing. The old binary is rejected by the real guard.
Old-kernel host-loader smoke also passes; these are **format/plumbing tests, not
new-kernel or Samsung HCE/DMA execution**.
Fresh ADTs intentionally carry fresh 256-byte random seeds; an initial smoke
comparison's false byte-identity assumption is preserved as a failure, and its
fixed fixture comparison normalizes only the seed without changing real ADTs.
Actual packaging requires the successful fresh CI kernel, HCE diagnostic
markers and the tested driver hash in its downloaded patched build tree.
Preparation/packaging JSON remain separate from finalized records.

### UFS probe 2 software delivery

**Ready test image:** `enxerto/out/starlte/boot-starlte-ufs-probe-2.img`,
**10459152 bytes**, SHA256
**`f8e7b3cbffd09a96e372d0267a19a247cd9365007e61df97295789c485b69bb3`**,
with adjacent `.img.sha256`. Hardware payload: **8410336 bytes**,
**`5d5632f08a3c1b55a6de25dd928dc757df2eb6cd679c82a32216146141a6c5bc`**.
The unchanged checked Enxerto Image is Samsung's kernel section; the SWEETXNU
payload is its ramdisk section, **not an iOS root filesystem**.

Actual packaging exits **0**. Complete production host-loader execution compares
**43778048 RAM bytes exact**, with dirty segment/ADT/boot_args destinations and
unchanged source payload; host-owned MMU/UI/jump services do not execute XNU.
The independent raw Samsung audit passes metadata, sizes, sections, legacy SHA-1,
unchanged second/DTBH/footer and standalone checksum. Both hardware/QEMU payloads'
**ten loaded kernel segments exactly match fresh CI Mach-O bytes**, including
bounded DEFLATE decoding and entry/memory bounds; neither contains a root RAMDisk.
Hardware RAM is `0x880000000 + 0x80000000`; QEMU RAM is
`0x50000000 + 0x60000000`, matching both ADTs and the generator.

Two auditor-only mistakes remain preserved as failures: the checkpoint-carried
stdin check guessed a different QEMU RAM window, and a first expanded check
applied loaded-segment size rules to nonloaded `__CTF` debug data. The original
stdin failure is distinguished from its saved read-only reproduction; corrected
audit **v2 exits 0**. No correction repackaged or changed the delivered image.

**Fresh real-kernel QEMU:** `ufs-probe-2-qemu-40955206538/`, **90 seconds**,
checker **0**, **7078 complete console bytes**, **7055 IRQs + one independently
verified same-PC-retried Skywalk demand fault**; no unexpected exceptions.
Fault: **ESR `0x96000046`**, **FAR `0xfffffe36107c0000`** (printed pageable-map
start), **ELR/retry `0xfffffe0007803c50`**, `_skmem_sysctl_init + 0x2c`, exact
`mov w8,#3; str w8,[x0]` bytes in both CI kernel and payload. Raw capture addresses,
ring, root/panic flags and CI source references are independently checked.
`rootvnode=0`, `early_panicking=0`, snapshot `_cpu_idle + 0x220`.

The separately viewed `fb.png` visibly shows **FAILED (6), writes 0, commands
failed 0 / PROBE HOLD**. Underlying runner remains **3 / blank_screen** because
the white-font decoder misses the readable coloured console. QEMU has **no
Samsung UFS host** and only checks missing-node failure before MMIO; it never
executes the new HCE path, FMP or storage commands. This is not a Samsung read,
root mount or userspace boot.

**Finalized bundle:** `enxerto/out/starlte/starlte-ufs-probe-2-40955206538.tar.gz`,
**30657282 bytes**, SHA256
**`be7c8247ffdb598925a954d867e88f822eb2475bdac6c774a959344654973714`**,
with adjacent `.gz.sha256`. Actual finalization with the reviewed fresh QEMU
capture exits **0**. Independent final verification passes all **148 manifest entries / 149
archive files**, standalone image/archive checksums, CI/source provenance and
failure preservation. All **351 source / 158 protected probe-1 hashes** match;
all **153 checked delivery/output paths** are ignored by git.

The frozen `research/starlte/ufs-probe-2-delivery/` includes raw-image/QEMU
reviews, source-profile evidence and the preserved failed records. Original
`source.json` / `delivery.json` retain preparation/packaging; separate
`source-final.json` / `delivery-final.json` record completion. The late independent
manifest/archive auditor and its passing review stay outside the completed
bundle, under `research/starlte/ufs-probe-2-bringup/`:
`independent-audit/delivery.py` and
`ci-40955206538-delivery-independent-review.json`. Delivery bytes are unchanged.

The user tested this delivery through the selected **TWRP / OrangeFox** route;
the new result is below. Samsung media reads remain the prerequisite for
preparing the iOS26 root.

### Photographed UFS probe 2 result

The user photograph received **2026-10-30** shows:

```text
STARLTE UFS: HCE enable-only attempt (inherited ready, idle lists)
STARLTE UFS: HCE enable result hce 0x1 hcs 0x10f is 0x0
STARLTE UFS: FMP DESCTYPE 3 result 0x0, TX/RX 0xc/0xc nexus 0xffff
SWEET UFS: command 0x00 timed out (is 0x4 hcs 0x10f dbr 0x0)
STARLTE UFS: LUN 0 scan result 60 (host stopped)
STARLTE UFS: probe FAILED (60), writes 0, commands failed 1
STARLTE UFS: PROBE HOLD - photograph this screen, then reboot to recovery
```

Runtime DMA is **`0x80208000+0x104000`**, inside the Image mapping
**`0x80164000+0x1ac000`**. Inherited CAP is **`0x1303ff0f`**, version **`0x210`**,
transfer/task DBRs **0**, both RSRs **1**, TX/RX **`0xc/0xc`**, Nexus **`0xffff`**,
SWRST **0**, GPIO **1**, CLKSTOP **0**, FORCE **`0xae0`**. These are runtime
observations, not addresses or settings to hardcode.

This physically corroborates the HCE correction and the SMC's zero return.
**TEST UNIT READY (`0x00`) transfers no media data**, so it establishes neither
PRDT/data-DMA acceptance nor readable storage. There is no capacity/GPT/sample,
root mount or userspace result yet. No installed-image checksum was supplied.
The original JPEG was located and directly viewed; its byte-exact copy,
hash/provenance and qualified transcription are under ignored
`research/starlte/device-ufs-probe-2/`.

**Source interpretation:** [Samsung `ufshci.h`][samsung-ufshci]:118 defines
**`IS=0x4` as UIC_ERROR**. Registers **`0x38/0x3c/0x40/0x44/0x48`** hold raw
PA/DL/NL/TL/DME layer codes. [`ufshcd.c`][samsung-ufshcd]:5392–5458 uses the
specific codes to distinguish recovery cases; `IS=0x4` alone does not select
a reset/retry policy. The current UFSHCI 2.1 command type **1** and per-command
Nexus setup agree with the reference; no particular descriptor-format bug has
been established.

The current timeout path writes `UTRLCLR` **before** printing DBR, and replaces
the recorded OCS with `0xff`. Thus **DBR=0 is post-cleanup**, not evidence of
natural completion. The `host stopped` suffix means software quarantine
(`su.dead`), not a measured hardware/list stop. The next additive probe will
capture first-error and terminal request/UIC/descriptor state before cleanup,
with bounded output and continued DMA quarantine.

Baseline driver/tests and completed probe-1/probe-2 evidence are preserved in
fresh `research/starlte/ufs-probe-3-bringup/`: seven source/documentation copies,
351 prior source hashes and **498 protected artifacts**. At this observation
milestone, no probe-3 driver patch, CI run or image has been produced.

### Probe-3 pre-cleanup diagnostics

Additive [patch 1134](../xnu/patches-12377/1134-starlte-ufs-precleanup-diagnostics.patch)
retains the inherited-host enable/read-only transport and adds bounded evidence:

* Save the **first observed pending interrupt error**, then the **terminal
  timeout or error-completion state** before request clear or IS acknowledgement.
* Print raw **PA/DL/NL/TL/DME**, HCE/HCS/IS, transfer/task list state and base,
  all **eight UTRD words** (including OCS) and the first **16 response-header
  bytes**. Observe DBR before and after each sample to expose completion races.
* Use volatile reads of the fixed owned Normal-NC descriptor arena and barriers;
  do not clean/write pending DMA, chase hardware descriptor pointers or dump media.
  These are explicitly **non-atomic live samples**, not completed-response proof.
* At most **two six-line samples** are emitted for a failed transport. Keep the
  existing five-second wait and bounded clear, permanent Exynos DMA quarantine,
  cached failures, read-only allowlist and pre-registration probe hold. No PHY,
  reset or UIC command write is added. Earlier observed errors remain fatal even
  if subsequent status/response fields look successful.

`ufs.py --out research/starlte/ufs-probe-3-check-final` exits **0**: **161 host
scenarios**, **24 ADT cases**, fatal ASan/UBSan and complete real-header Mach-O
compilation. The **26 added cases** check read-clear UIC registers, DMA mutation
by cleanup, all seven error bits, pending/completed/no-error paths, deadline and
DBR races, unknown raw bits, bounded output, sleepable waits, failure caching,
late-DMA quarantine and the photographed TUR failure hold. The unchanged host
service boundary does not model Samsung clocks/link/firmware or physical DMA.

Two initial **fixture** failures are preserved: a deadline injection was one
model tick too late, and a synthetic `longjmp` from the nonreturning hold bypassed
the caller's cached-errno assignment. Corrected expectations pass without driver
changes. The first forward-export fixture also lacked its own git repository
and skipped paths; the fresh isolated fixture now verifies exact final bytes.

Driver SHA256 **`06029905a8bf3f7ed0126d1728f534b20f2af2c8d9386237abdcf9dd7924e7d8`**;
patch **6088 bytes**, SHA256
**`ddf2dff41396c6a233a4bf591cca17c32c3ea3224001782ef4dbbeeb712766de`**.
Exact GNU export, forward/reverse application and **498 protected-artifact
hashes** pass. Fresh `verify-series.py --out research/starlte/series-check-ufs-probe-3`
exits **0**: **four staging scripts / 277 patches / 6369 source files exact**,
including the 14 pre-1131 and one pre-1132 baseline hashes; only the two documented
dxb skips. Implementation **`4d12ded8d3b18048e8e5e8f685a997c8a1983f59`** and
the physical-observation commit **`de43bcb`** are pushed. Explicit Intel build
[41014066730](https://github.com/friendlycatt/Bergamota/actions/runs/41014066730)
uses **26.5 / DEVELOPMENT / SWEET / Xcode 26.1 / use_kdk=true / keep_going=true**,
source **4d12ded**, job **123926261213**, initially queued at dispatch. The retained
watcher now exits **0**: **SUCCESS**, attempt **1**, job completed
**2026-10-30T07:11:15Z**, run updated **07:11:16Z**. Exact durations are
**41m 12s Build / 44m 52s job / 45m 02s dispatch-to-job-completion**. Actions confirm
**x86_64 / Xcode 26.1.1 / 17B100**, actual host macOS **26.6.1 / 25G76**; the kernel
target remains **26.5 / DEVELOPMENT / SWEET**. The error summary has **zero lines
in errors.txt / no failed objects**; raw `error:` substrings include expected
patch-check skips and interleaved warnings, not all compiler failures.
Artifacts **xnu-kernel 12960019646 / 174382572 ZIP bytes** and
**xnu-buildtree 12960388132 / 30964317 ZIP bytes** are unexpired. Fresh downloads,
complete source-profile replay and actual kernel inspection are active; no
probe-3 image is available yet. The current phone result remains probe 2's timeout.

Fresh isolated preparation in `research/starlte/ufs-probe-3-bringup/` pins the
new source/run/output paths and refuses existing one-shot outputs. The prepared
delivery contains the unchanged checked **1740800-byte Image** and ELF, fresh
hardware/QEMU ADTs, zero hardware DMA/Image placeholders and no QEMU UFS node.
All **352 tracked source hashes** match **4d12ded**; all **498 protected
probe-1/probe-2 files** and the old reproduction suite remain unchanged.

The complete new downloader/profile/inspection/package/capture/finalizer/test
suite and three independent auditors pass **13 syntax checks / 25 unittest
methods with adversarial subtests**. The source profile expects **6374 files**,
derived from the new local reconstruction, six exact dxb substitutions and
**nine previously reviewed/hash-pinned external-header copies** prepared before
examining the new CI archive. No exclusions or learned-from-archive expectations,
independent SDK/KDK reinstall or byte-identical kernel rebuild are claimed.
The unchanged exception policy is AST-verified against both older captures.

A **SYNTHETIC format-only** finalizer fixture passes **ten negative guards /
95 manifest files / exact archive hashes**, explicitly mocking old-kernel,
archive-pin and full-source-proof boundaries; the strict metadata guard remains
active and real marker checks reject both older kernels. An old-probe-2 format
smoke also passes **43778048 host-loader RAM bytes exact**, dirty destinations,
fresh seeds/source unchanged and independent Samsung section/metadata/SHA-1
checks. These are **not probe-3 CI, Samsung storage or phone execution results**.
Real artifacts must still be downloaded/reconciled, packaged, captured for
90 seconds in QEMU, visually reviewed and independently audited before delivery.

## Getting-started guide review for S9

The pasted guide describes Poco F1 / Qualcomm and older iOS18 defaults.
For this checkout and phone:

| Guide step | S9 / iOS26 equivalent |
| --- | --- |
| 0: Qualcomm appended DTBs | The supplied Samsung template has a separate DTBH section. Use the [Samsung packager](#samsung-bootimg-from-a-template). |
| 1: XNU11215 on `ry-diffusion/Bergamota` | XNU12377, patches 1131–1134, fork `friendlycatt/Bergamota`; dispatch with `macos_version=26.5`. Run **40826795487** predates UFS, **40931557218** predates the enable correction, **40955206538** predates pre-cleanup diagnostics. |
| 2: iPhone X identity and beryllium hardware | `tools/mkadt.py --board starlte --identity n104ap`; the current Apple profile is N104/iPhone 11. |
| 2b: Adreno 630 firmware | S9 Exynos uses Mali; the Qualcomm firmware/GPU path does not apply. |
| 3 / 3b / 3c: Renegade root and prebuilt tools | Relevant root-preparation workflow, using `RENEGADE_TARGET=ios26`; prebuilt user tools do not add Samsung kernel drivers. |
| 4: `mkboot-full.sh` | This helper explicitly forces `BOARD=beryllium` at line 32. S9 uses `mkpayload.py`, the starlte loader and `mkboot-starlte.py`. |
| 5: Poco ABL/Bergamota fastboot flashing | The current S9 route is TWRP / OrangeFox with Samsung boot images. Bergamota's Qualcomm USB/UFS flashing path is not established on this phone. |

[`targets.py`](../tools/renegade/targets.py) selects **iOS 26.6.2 / 23G90 /
iPhone12,1 / N104AP** by default. The expected root output is
`research/ios26/ios26-root-full.simg`. Preview the Python root workflow before
downloading anything:

```sh
RENEGADE_TARGET=ios26 RENEGADE_DRY_RUN=1 tools/renegade/renegade root
```

The reviewed command exits **0** on this Linux host. The preview now uses
profile-derived decrypted DMG names when the manifest is absent and respects
dry mode for macOS excluded-tree copies and signer entitlement files. Real
builds still require `BuildManifest.plist`; a downloaded manifest takes
precedence in previews too. The platform suite passes **24 tests**, with one
macOS-only signer-parity test skipped; its four full-preview cases cover
ios18/ios26 × Linux/macOS without work/image creation or subprocess execution.

This host is **CachyOS (Arch-based)**, so the guide's `apt install` line needs
distribution-specific packages. At review time it had about **61 GiB free**,
but `ipsw`, `apfs-fuse`, `mkfs.hfsplus`, `fsck.hfsplus` and optional
`dropbearkey` were absent from PATH. After those prerequisites are supplied,
the root-preparation sequence is:

```sh
RENEGADE_TARGET=ios26 tools/renegade/renegade fetch-tools
RENEGADE_TARGET=ios26 tools/renegade/renegade fetch
sudo -E env RENEGADE_TARGET=ios26 tools/renegade/renegade root
```

No real root build has been performed in this checkout. The default raw image
is 20 GiB logical size, with additional space needed for source volumes and
scratch files. Linux mounting/formatting and the resulting filesystem still
need validation with the actual IPSW.

Once Samsung storage is working, root selection can use `rd=ufsroot0`
(`ufsroot=userdata` is the default partition); arm64e userspace also needs
`sweetarm64e=1`. The full boot arguments need an S9-specific review before
enabling writable root/userland. Installation targets the existing userdata
partition, with confirmation before its destructive write as required by
`AGENTS.md`.

Local evidence is in ignored `research/starlte/root-bringup-review/`: the
user observation, payload/ADT and source audit, original failing preview,
fixed preview and platform-test logs. That root-guide review used kernel CI
**40826795487** and created no boot image or hardware result; it predates the
new UFS probe above.

## Next milestones

1. **Read-only Samsung storage.** Diagnose the photographed first-command
   UIC timeout with a bounded pre-cleanup diagnostic probe. Obtain actual LUN
   capacity/GPT and repeated userdata reads
   before preparing the iOS26 root. A userdata write needs explicit confirmation.
2. **Complete the physical XNU record.** Probe 2 establishes HCE enable and
   the FMP return, followed by a first-command UIC timeout and failure hold.
   Capture whole-screen results of subsequent tests, including the last visible
   line if enable or storage discovery stalls.
3. **Timer and CPU delivery.** Validate CNTV/INTID27 on hardware, then
   CPU_ON/IPIs on the real affinities. The ADT supplies 26 MHz despite the
   observed zero `CNTFRQ_EL0` register.
4. **Complete live hardware facts.** Resolve RAM reservations/firmware
   allocations and the inherited display's stride/format/IOMMU state.
   Keep dumps and device identifiers under ignored `research/`, `dtb/`
   or `backup/` paths.
5. **Console follow-up.** Complete framebuffer-serial initialization when
   it is the only registered interface; teach QEMU diagnostics to account
   for the themed video console or consume the captured log ring.
6. **Root boot and Samsung peripherals.** Validate the root mount and
   userspace startup, then sustained display, USB, input and power support.
   Mali acceleration is a separate hardware-driver effort.

[samsung]: https://github.com/duhansysl/exynos9810-kernel/blob/914eabe7b6799ab0eb159360a5f9731dd50d0f1d/arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts
[samsung-ufs]: https://github.com/duhansysl/exynos9810-kernel/blob/914eabe7b6799ab0eb159360a5f9731dd50d0f1d/drivers/scsi/ufs/ufs-exynos.c
[samsung-ufshcd]: https://github.com/duhansysl/exynos9810-kernel/blob/914eabe7b6799ab0eb159360a5f9731dd50d0f1d/drivers/scsi/ufs/ufshcd.c
[samsung-ufshci]: https://github.com/duhansysl/exynos9810-kernel/blob/914eabe7b6799ab0eb159360a5f9731dd50d0f1d/drivers/scsi/ufs/ufshci.h
[samsung-fmp]: https://github.com/duhansysl/exynos9810-kernel/blob/914eabe7b6799ab0eb159360a5f9731dd50d0f1d/drivers/crypto/fmp/fmp.c
[mu]: https://github.com/Project-Silicium/Mu-Silicium/tree/7a2ada19841a129f86fb791c0b9a710436c849a4
[mu-dts]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Resources/DTBs/starlte.dts
[mu-memory]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Platforms/Samsung/starltePkg/Library/MemoryMapLib/MemoryMapLib.c
[mu-board]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Platforms/Samsung/starltePkg/starlte.dsc
[mu-soc]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Silicon/Samsung/S5E9810Pkg/S5E9810Pkg.dsc.inc
[mu-sec]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Silicon/Samsung/S5E9810Pkg/Library/PlatformSecLib/PlatformSec.c
