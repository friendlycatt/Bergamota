# Galaxy S9 / Exynos9810 bring-up

Target: Samsung Galaxy S9 **starlte** (Exynos9810, SM-G960F / G960FD),
using Bergamota's iOS 26 / XNU12377 stack. The Snapdragon S9 and S9+ have
different board descriptions.

**Status, 2026-10-30:** the [probe-3 photograph](#photographed-ufs-probe-3-result)
shows first **PA `0x80000010`**, then **PA=0**, with both pre-cleanup samples
reporting **IS=4 / HCE=1 / HCS=0x10f / DBR 1->1 / OCS=0x0f**. The first
TEST UNIT READY still times out: **FAILED (60), writes 0, commands failed 1 /
PROBE HOLD**. The source/descriptor review is complete; the timeout cause
and PA indication's timing remain unclassified. The active
[startup/NOP candidate](#opt-in-link-startup-and-nop-candidate), patch **1136**
on `starlte/ufs-link-startup`, addresses the missing post-HCE transport sequence:
**329 host scenarios / 24 ADT cases / real-header compile** and
**279-patch / 6369-file reconstruction PASS**. Candidate CI/image and physical
validation are next. Probe 4's [boundary experiment](#selected-next-read-only-experiment)
remains pinned to **2347574 / CI 41072462470** in the main checkout.
**Samsung media reads and an iOS root mount
remain unverified**.

[Probe-3 software delivery](#ufs-probe-3-software-delivery) is complete:
Intel CI **41014066730 / 4d12ded** succeeded; all **6374 source files**,
diagnostic binary, Samsung/payload audit and corrected QEMU audit verify.
Actual finalization and independent delivery audit both exit **0**,
all **189 manifest entries / 190 archive files exact**. The physical
[display probe](#display-probe-1) reached **PROBE READY**, and the full XNU26
CI build and QEMU VM/CPU/IOKit initialization are verified. Timer delivery,
touch, Mali GPU, USB and power-management support remain follow-up work.

The port reuses Heartwood, Renegade's arm64e/PAC rewriting, and the kernel PAC
fallback in patch 1019. The platform and storage work is in
[`1131-starlte-exynos9810-bringup.patch`](../xnu/patches-12377/1131-starlte-exynos9810-bringup.patch),
[`1132-starlte-readonly-ufs.patch`](../xnu/patches-12377/1132-starlte-readonly-ufs.patch),
[`1133-starlte-enable-inherited-ufs-host.patch`](../xnu/patches-12377/1133-starlte-enable-inherited-ufs-host.patch),
[`1134-starlte-ufs-precleanup-diagnostics.patch`](../xnu/patches-12377/1134-starlte-ufs-precleanup-diagnostics.patch),
[`1135-starlte-ufs-presubmit-boundaries.patch`](../xnu/patches-12377/1135-starlte-ufs-presubmit-boundaries.patch)
and [`1136-starlte-ufs-link-startup.patch`](../xnu/patches-12377/1136-starlte-ufs-link-startup.patch),
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
| UFS (`ufs.py`) | PASS, 329 byte-level host scenarios with fatal ASan/UBSan, 24 ADT ownership/bounds cases, complete real-header Mach-O compilation; includes startup/NOP and deferred boundary output |
| Ordered patch reconstruction | PASS: candidate four-stage / 279-patch reconstruction, 6369 files and baseline hashes exact; first attempt stopped on BearSSL DNS failure before patching; prior probe-3 CI profile reconstructs all 6374 files with six dxb substitutions and nine prior-reviewed header copies; two documented dxb skips |
| Full XNU26 CI build | PASS, run 41014066730 / 4d12ded; actual ARM64 Mach-O, checksum, pre-cleanup markers and patches 1131–1134 verified |
| Full hardware payload host loading | PASS, production C loader/FDT/inflater; 43778048 loaded bytes match the Python reference, including dirty BSS/boot_args destinations |
| Final Samsung image | PASS, exact loader/payload sections, preserved template metadata/DTBH/footer and independently verified SHA-1/SHA256 |
| Full-kernel QEMU | Probe-3 checker and corrected independent audit 0 after 90 seconds: expected missing-node FAILED/PROBE HOLD, 7078 console bytes, 6920 IRQs and one verified retried Skywalk fault; framebuffer-only decoder exits 3; Samsung UFS not modeled |
| Renegade platform/root preview | PASS, 24 tests and one macOS-only signer-parity skip; includes ios18/ios26 × Linux/macOS full previews before downloads |

Patch 1131 is 25669 bytes across 12 files, SHA256
`cac36bd98fc5ed6c1c6cab7de24bddc069fc62cf16f705d073a9920374c890bf`.
Patch 1132 is 34532 bytes for `bsd/dev/sweet_ufs.c`, SHA256
`6ee211f1f329c2f572dde7cc614780227da45b11e21bdaf9dfed9299164587a6`.
Patch 1133 is 6172 bytes, SHA256
`aeb561be8a745543a00dd7216140b45ea49e90bb30a10231c0923709c809c73b`.
Patch 1134 is 6088 bytes, SHA256
`ddf2dff41396c6a233a4bf591cca17c32c3ea3224001782ef4dbbeeb712766de`.
Patch 1135 is 5410 bytes, SHA256
`e0a8b80e59b9d2f898b589429ed3cf6085c422d24a4df69d32a00cbad68bda47`.
Patch 1136 is 20834 bytes, SHA256
`df98fd4685a9b3d6921d2b235c7bb1f374787f6b8234a278ff75aa623ef7d30f`.
The reconstruction uses XNU `xnu-12377.121.6` at
`ac9718fb1af618d5ce8678d0dc6e8a58f252216f` and dxb at
`663906528d1cd5ea8fde7de6f16de73db9511549`. The latest local reconstruction
is retained in the startup worktree's ignored
`research/starlte/series-check-link-startup-retry1/`; the prior reviewed
CI-profile replay is in
`research/starlte/ufs-probe-3-bringup/ci-41014066730-source-profile/`.
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
| UniPro confirmation | `GET_GENERRCODE_DIRECT`, LINKSTARTUP result at `0x11110000 + 0x7854` | Exact `unipro-reg=<0x11110000,0x8000>`; opt-in startup reads direct result after UIC completion |
| Descriptor units | `UFSHCD_QUIRK_PRDT_BYTE_GRAN`, `ufs-exynos.c:559`; [ufshcd.c][samsung-ufshcd]:1695,3235 | Byte-based response/PRDT offsets and lengths |
| PRDT layout | `CONFIG_EXYNOS_FMP=y`; [ufshci.h][samsung-ufshci] and [fmp.c][samsung-fmp] | 128-byte entries, zero encryption selectors, DESCTYPE 3 via `SMC_CMD_FMP_SECURITY(0, 0, 3)` |
| PRDT data unit | `PRDT_SET_SIZE(12)` at `ufs-exynos.c:442–444` | TX/RX low five bits set to 12: log2(4096), **not** a 12-byte descriptor stride |
| Request quirks | Positive-bit `UTRLCLR`; vendor Nexus register offset `0x40` | Clear slot 0 with `1`; set/verify Nexus for SCSI, clear/verify it for the opt-in NOP |
| DMA addressing | Initial `DMA_BIT_MASK(32)`; `ufshcd_set_dma_mask()` can select 64-bit addressing when CAP.64AS permits | Use an Image-owned DMA32 arena even though XNU manages upper RAM |

Probe 2 observes a zero return from the descriptor-format SMC. Actual data DMA,
descriptor acceptance and the failing link's condition remain unmeasured.
The default implementation reuses inherited host/link configuration. The
opt-in startup candidate initializes transport after its own HCE enable,
while retaining inherited PHY calibration and quarantining transport failures.

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

Probe 2's timeout path writes `UTRLCLR` **before** printing DBR, and replaces
the recorded OCS with `0xff`. Thus **DBR=0 is post-cleanup**, not evidence of
natural completion. The `host stopped` suffix means software quarantine
(`su.dead`), not a measured hardware/list stop. This motivated the additive
pre-cleanup diagnostics below, with bounded output and continued DMA quarantine.

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
complete source-profile replay and actual kernel inspection now exit **0**.
The new kernel is **22213504 bytes**, SHA256
**`f9005570abeb56999f37d4a8879c2873442c45f2351e1fb262789228e7f16f2f`**:
ARM64 DEVELOPMENT/SWEET, CI checksum, all new diagnostic markers/symbols and
zero-byte errors.txt verified. The actual **31280071-byte build-tree TGZ** is
pinned as **`5c1615f905a6016f060121e8420cc60b5e861ee9e659ff0bb75a9b85ed1b8f3c`**.
The five-stage **277-patch** replay and actual archive match **6374/6374 files**,
with no missing/different/additional files or exclusions. All **352 source
hashes / 498 protected artifacts** remain unchanged. The entire **108-line
link-errors.txt** was viewed: interleaved compiler-warning fragments plus
duplicate-library/segment-order linker warnings, no established linker failure.
Probe 3 is now packaged locally as **`boot-starlte-ufs-probe-3.img`**, **10461200
bytes**, SHA256 **`2f17f8dc2ba9fe5a4ed5bfb838c6e2b4c476d4a4615384e7c745ea6d276f07b4`**.
Production host-loader validation compares **43778048 RAM bytes exact**, with
dirty destinations and the source payload unchanged; it does not execute XNU.
Preparation records, Image/ELF and fresh ADTs are unchanged. Independent raw-image
verification now exits **0**: Samsung metadata, 2048-byte page, legacy SHA-1,
unchanged second/DTBH/footer and standalone checksum PASS. The DTBH has one
Exynos9810/revision 26–255 entry and a **301372-byte FDT**. Hardware and QEMU
payloads are **8411984 / 8405952 bytes**; all **ten loaded segments in each**
match the actual CI Mach-O bytes, including bounded raw DEFLATE, entry/end/VA
and physical bounds. No iOS root RAMDisk is present; fresh seeds and hardware
DMA placeholders are preserved. The fresh **90-second** QEMU capture completed
in `research/starlte/ufs-probe-3-qemu-41014066730/`: checker **0**, **7078 raw
console bytes**, **6920 IRQs + one verified retried Skywalk demand fault**,
`rootvnode=0`, `early_panicking=0`. The actual screenshot shows **FAILED (6),
writes 0, commands failed 0 / PROBE HOLD**. The underlying white-font runner
still exits **3 / blank_screen**. The initial independent auditor **failed (1)**
at its idle-only snapshot assertion: PC is **`_sweet_gic_ack_irq + 0x18`**, not
`_cpu_idle + 0x220`. CI source/binary/payload verification now proves the exact
post-IAR instruction site, **INTID 27**, direct caller **`_sleh_irq + 0x34`**,
masked EL1t state and terminal IRQ from **`_machine_idle + 0xf4`**. The corrected
guard accepts only this exact IRQ site or the unchanged exact idle site; no later
IRQ return or new exception permission is claimed. **36 unittest methods / 13
script syntax checks** and a fresh **15-negative-guard SYNTHETIC finalizer fixture**
pass with reconciled script pins. The raw auditor abort loop and capture checker
remain AST-identical to their originals. The corrected real raw audit **v2 also
failed (1)**: its line 271 incorrectly requires an early GIC `kprintf` message
in the captured console; no such message is present in the complete ring. This
led to the fixed-CI-source logging review and v3 correction below.
Both failed audits, 20 original files, 23 preserved v2 files, all 13 raw capture
hashes and both fixtures remain intact; no QEMU rerun. The source review confirms
that early `kprintf` output is conditional, not an unconditional console-ring
record; the exact runtime reason for this message's absence is not measured.
The v3 read-only real-main test then exposed a screen-result/DEFLATE variable
collision in report construction; that failed guard and 18 source/evidence files
are preserved too. Only the screen-result variable outside the abort loop was
renamed. Fresh **38 unittest methods / 13 script checks / 15 SYNTHETIC negative
guards** pass, with **189 fixture manifest entries / 190 archive files exact**.
The separately recorded **real v3 independent audit exits 0**, validates all
eight CI-source references and all 13 unchanged capture hashes, and retains the
unchanged exception and narrowly verified IRQ-site policies. The final paused
IRQ's return is still unobserved. Real finalization and independent archive
verification are complete, as recorded below. The subsequent
[probe-3 photograph](#photographed-ufs-probe-3-result) supplies the physical result.

Fresh isolated preparation in `research/starlte/ufs-probe-3-bringup/` pins the
new source/run/output paths and refuses existing one-shot outputs. The prepared
delivery contains the unchanged checked **1740800-byte Image** and ELF, fresh
hardware/QEMU ADTs, zero hardware DMA/Image placeholders and no QEMU UFS node.
All **352 tracked source hashes** match **4d12ded**; all **498 protected
probe-1/probe-2 files** and the old reproduction suite remain unchanged.

The complete new downloader/profile/inspection/package/capture/finalizer/test
suite and three independent auditors initially passed **13 syntax checks / 25
unittest methods**; the corrected audit suite now passes **38 methods** with
adversarial and real-main read-only checks. The source profile expects **6374 files**,
derived from the new local reconstruction, six exact dxb substitutions and
**nine previously reviewed/hash-pinned external-header copies** prepared before
examining the new CI archive. No exclusions or learned-from-archive expectations,
independent SDK/KDK reinstall or byte-identical kernel rebuild are claimed.
The unchanged exception policy is AST-verified against both older captures.

A preserved initial **SYNTHETIC format-only** finalizer fixture passed **ten negative
guards / 95 manifest files / exact archive hashes**, explicitly mocking old-kernel,
archive-pin and full-source-proof boundaries; the strict metadata guard remains
active and real marker checks reject both older kernels. An old-probe-2 format
smoke also passes **43778048 host-loader RAM bytes exact**, dirty destinations,
fresh seeds/source unchanged and independent Samsung section/metadata/SHA-1
checks. These are **not probe-3 CI, Samsung storage or phone execution results**.
Real artifacts now pass download/reconciliation/inspection, Samsung packaging and
independent raw-image verification and the capture checker/actual visual review.
The corrected real IRQ-snapshot audit, finalization and independent archive
review pass. Neither failed audit was relabeled. QEMU has no Samsung UFS and
cannot validate HCE/FMP/UIC/data DMA.

### UFS probe 3 software delivery

**Complete, independently verified read-only diagnostic delivery** for the
user-identified **SM-G960F / G960FD**, not an iOS installation or root/userland boot.

**Test image:** `enxerto/out/starlte/boot-starlte-ufs-probe-3.img`, **10461200 bytes**,
with adjacent `boot-starlte-ufs-probe-3.img.sha256`:

```text
2f17f8dc2ba9fe5a4ed5bfb838c6e2b4c476d4a4615384e7c745ea6d276f07b4
```

**Final bundle:** `enxerto/out/starlte/starlte-ufs-probe-3-41014066730.tar.gz`,
**30972661 bytes**, with adjacent `starlte-ufs-probe-3-41014066730.tar.gz.sha256`:

```text
11eca4f8009c86fe2557c300e3b663e8e8ce1c95908a3222fe7016412ba5b42e
```

The real `finish_probe.py --qemu-out research/starlte/ufs-probe-3-qemu-41014066730 --screen-reviewed`
exits **0**, **2026-10-30T08:36:20–08:36:27Z**. Separately
executed `independent-audit/delivery.py` exits **0**, **08:38:53–08:38:59Z**:
all **189 actual manifest entries / 190 archive files**, both standalone
checksums, **352 source hashes / 498 protected prior artifacts / 13 reproduction
scripts / 194 ignored artifact paths** verify. No mocks were used in either
actual command; matching counts in the synthetic fixture are not the delivery proof.

Frozen `research/starlte/ufs-probe-3-delivery/` contains the image, CI kernel,
loader/payloads/ADTs, exact CI-source evidence, raw QEMU capture and independent
raw-image/QEMU audits, plus the preserved failed v1/v2 audits and failed v3
reporting guard. Preparation/packaging `source.json` / `delivery.json` remain
separate from `source-final.json` / `delivery-final.json`. The manifest covers
every other file. The **late independent delivery-audit results and final
documentation remain outside the frozen bundle**; no archive or delivery file
was changed for them.
Both wrappers verify all **35 preexisting packaging files / 13 raw capture
hashes / 20 original files / 23 v2 files / 18 failed-guard files**, the **158-file
original / 233-file v2 fixtures** and current v3-fixed synthetic fixture unchanged.

Exact command/log/result records are in ignored
`research/starlte/ufs-probe-3-bringup/`:
`ci-41014066730-finalization-command.json`, `ci-41014066730-finalization.log`,
`ci-41014066730-finalization-result.json`, `ci-41014066730-delivery-final.json`,
and `ci-41014066730-delivery-independent-{command,result,review}.json` plus
`ci-41014066730-delivery-independent.log`. The independent delivery auditor is
pinned to **`5c3bab78b510ce73dfb825d8ba4ee13a55393645de5062678b00f276af7db04d`**;
its actual report/log SHA256 is
**`47471b8df0781f6a87e75c0937527d727dbab0514c68c83109a0c80b734c345d`**.
**Do not rerun** any completed CI/download/packaging/capture/audit/guard/fixture
or finalizer path, or edit the frozen delivery/archive.

The user's [probe-3 photograph](#photographed-ufs-probe-3-result) is now received
and interpreted. The selected test-image route remains **TWRP / OrangeFox**.
Commands stay read-only even with `ufsrw=1`; the bounded wait/clear, permanent
quarantine and pre-BSD/root hold remain. Preserve recovery, bootloaders, modem,
persist and partition backups; do not put this boot image in userdata or other
non-boot partitions.

The agent performed **no phone operations**. Samsung descriptor/data-DMA
acceptance, media capacity/GPT and repeated userdata reads remain unverified.
No Apple root was downloaded or built, and no root mount/userspace proof exists.
Root preparation follows measured Samsung reads; any destructive operation on
the existing userdata partition needs separate confirmation, with no GPT changes.

### Photographed UFS probe 3 result

The original user JPEG was located and directly viewed on **2026-10-30**:
**230972 bytes**, SHA256
**`dc0f6b9493bee7dc69d763a522c33429cdba10cc88381deb8d98a8c8e68d3102`**.
Its byte-exact copy, three nearest-neighbor crops and curated UFS-tail
transcription are in ignored `research/starlte/device-ufs-probe-3/`.
The screen contains probe 3's new markers; an installed-image checksum was
not supplied. The HCE enable again reaches **1 / HCS `0x10f` / IS=0**, and
the FMP DESCTYPE 3 call returns **0**. The two live samples show:

| Field | First error | Timeout, before request clear |
| --- | --- | --- |
| Command / LUN | TUR `0x00` / 0 | TUR `0x00` / 0 |
| IS / HCE / HCS | `0x4` / `1` / `0x10f` | `0x4` / `1` / `0x10f` |
| PA | `0x80000010` | `0` |
| DL / NL / TL / DME | All zero | All zero |
| Transfer DBR before/after sample | `1 -> 1` | `1 -> 1` |
| Transfer RSR / task DBR / task RSR | `1 / 0 / 0` | `1 / 0 / 0` |
| UTRL base | `0x80140000` | `0x80140000` |
| OCS / sampled 16-byte response header | `0x0f` / all zero | `0x0f` / all zero |

After request clear the screen reports **DBR=0**, **FAILED (60), writes 0,
commands failed 1 / PROBE HOLD**. The request was still pending at both sampled
boundaries. These are non-atomic samples saved before cleanup and printed
afterward. The printed **`post-clear is 0x4`** is saved after `UTRLCLR` but
**before IS acknowledgement**; DBR=0 is cleanup, not natural completion.
Timeout statistics substitute `last_ocs=0xff`, distinct from the sampled **0x0f**.

Runtime pool **`0x80140000+0x104000`** ends at **`0x80244000`**, within the Image
mapping **`0x8009c000+0x1ac000`**, ending at **`0x80248000`**. Inherited UTRL base
is **`0x00000000ef002000`**, CAP **`0x1303ff0f`**, version **`0x210`** and FORCE
**`0xde0`** (probe 2 showed `0xae0`). These are this boot's observations, not
addresses/settings to hardcode or proof of controller DMA access.

**Independent UTRD decoding:** both samples contain these eight little-endian
words, consistent with the actual CI driver's current layout:

```text
11000000 00000000 0000000f 00000000 80141000 00000000 02000200 04000000
```

* DW0: command type **1 / UFS_STORAGE**, interrupt-command bit set, **no data**
  direction. Samsung selects this command type for UFSHCI 2.1
  ([`ufshcd.c`][samsung-ufshcd]:1952–1966).
* DW1/DW3: reserved zero; DW2: **OCS_INVALID_COMMAND_STATUS `0x0f`**, the
  software initializer. No completed success or descriptor-error status appears.
* DW4/DW5: UCD **`0x80141000`**, pool + `0x1000`, aligned to 128 bytes.
* DW6: response **offset 512 / length 512 bytes**, address **`0x80141200`**.
* DW7: PRDT **offset 1024 / length 0 bytes**, address **`0x80141400`**.
  Samsung's byte-granularity quirk applies; TUR has no data PRDs.

The 1536-byte local UCD ends at `0x80141600`, before bounce data at `0x80144000`.
Visible software descriptors, HCS readiness and a zero FMP return cannot
distinguish no controller fetch from fetched-but-unfinished work. Only 16
response bytes were sampled; **TUR transfers no media data**.

**PA interpretation:** [Samsung `ufshci.h`][samsung-ufshci]:171–172 defines the
valid bit 31 and code mask `0x1f`. The per-bit names come from supplemental
[Linux v6.6 `ufshci.h`][linux66-ufshci]:235–236: lane mask `0xf`, generic bit
`0x10`. Its [core driver][linux66-ufshcd]:6592–6605 calls the generic indication
**LINERESET**. No lane bits or separate **DL.PA_INIT bit 13** are present in
the samples. Linux also explicitly reads UECPA to clear a LINK_STARTUP
indication (4897–4898). PA becoming zero is consistent with read-clearing;
this photograph does not establish the clearing mechanism or event timing.
The initial/post-enable IS=0 readings did not sample the layer-code registers.
The photograph supplies neither a measured timeout duration nor a hardware
virtual-timer trace, and does not classify the timeout's cause.

**Layout comparison:** the pinned Samsung header uses **1024-byte** command
and response slots, with response/PRDT offsets **1024/2048**. The
[Samsung history change][samsung-upiu1024] (`a4bd396`, `Merge N960FXXU9FVH1`)
changes the previous 512-byte value; older Samsung and Linux v6.6 use **512**.
A local 1024-byte-slot TUR would encode DW6/DW7 **`04000400 / 08000000`** and
use **2560 bytes** with four 128-byte PRDs, fitting the **12288-byte** reserved
UCD space before the bounce buffer. This is a source compatibility difference,
not a demonstrated hardware requirement or tested fix. Retain it for a later
one-variable comparison after the directly observed PA/UIC timing gap is measured.

The one-shot `interpret_photo.py` exits **0** at **12:33:34Z**. Its complete
`descriptor-review.json` was directly reviewed, SHA256
**`5209f41a048873cbe19bb8ba17962bfd86979b7f914fa16a116b594cbd93eea4`**.
`reference-provenance.json` pins 18 source/provenance files, the actual CI driver
and Samsung history; supplemental Linux is commit
**`ffc253263a1375a65fa6c9f62a893e9767fbebfa`**. Full source headers are retained.
These records and the physical evidence remain outside the frozen delivery.

#### Selected next read-only experiment

**`starlte-pre-submit-uic-boundaries-v1` — implemented and locally validated
in patch 1135, committed as 2347574.** Its CI **41072462470** attempt 1 lost the
runner at **15:17:40Z** without artifacts; one same-source retry was accepted
at **15:32:28Z**. Its delivery remains in the main checkout. Keep the **512-byte UPIU layout**
to measure whether the indication is already recorded before command submission:

1. After idle/list-stop verification, sample at the existing **pre-enable
   check**, before HCE enable. Sample this boundary on an already-enabled host
   too, retaining its enable-skipped behavior.
2. After descriptor/Nexus preparation, sample **immediately before the first
   TUR doorbell**. Both samples record raw **IS/HCE/HCS**, both **DBRs** and
   **PA/DL/NL/TL/DME** from the already mapped standard HCI window.

The exact Exynos identity plus **`ufsprobe=1`** gates this experiment; absent,
zero or non-one probe values and Qualcomm do not add boundary reads. Persist
the two raw ten-register samples and their result in **`su.boundary[2]`**, with
per-boot capture/output masks. Allow at most **two new two-line records per
boot**, alongside patch 1134's unchanged at-most-two six-line samples.
**Defer both records to `ufs_probe_hold()`**, reading retained RAM only: no
console operation intervenes between the final sample and first TUR doorbell.
TUR retries and LUN scans do not repeat these samples.

Latch all values before any subsequent acknowledgement. A sampled **IS error
or any nonzero UIC error-code register → EIO**, either sampled pending DBR
**→ EBUSY**, and sampled readiness loss or pre-TUR HCE loss **→ ENXIO**. Each
failure permanently quarantines the host and reaches the existing hold
**before the next setup action/doorbell**; later zeros cannot erase the
earlier evidence. A pre-submit rejection correctly reports **zero submitted
commands failed**, not a TUR timeout. Keep Image-owned DMA32/Normal-NC, FMP-bypass
descriptors, pending-request checks, the five-second deadline/bounded positive-bit
clear, permanent quarantine, cached failures and pre-BSD/root hold. Exynos
remains read-only even with `ufsrw=1`. Add no reset/link/PHY/clock operation,
retry after quarantine, descriptor-pointer chase, pending-DMA clean or media dump.

The first boundary follows the existing takeover IS acknowledgement/list stop;
it is not an untouched bootloader-entry snapshot. Earlier rejection can prevent
it from being reached, and its own failure intentionally prevents the second
sample. MMIO reads may clear latches and affect timing; these sequential samples
are not atomic or passive. A first-boundary indication predates the later TUR;
a new second-boundary indication narrows it to setup; one first seen while TUR
is pending narrows it to after the final boundary, without proving command causality.

Fresh local validation **exits 0**, **13:49:56–13:50:02Z**: **236 host scenarios /
24 ADT cases / fatal ASan/UBSan / complete real-header Mach-O compile PASS**;
compiler log empty. The 75 added scenarios independently trace all ten reads,
raw 512-byte TUR descriptors, no setup/submission after rejection, read-clear
IS/layer faults, readiness/pending races, output budgets, retries/scans,
cached failure/late DMA, probe-value gates and Qualcomm regressions. The host
hold uses a test-owned `longjmp` before `su.init_error` assignment; subsequent
EIO checks the cached attempt plus `su.dead`, not a return from the real hold.
Exact tested inputs and results remain in ignored **`ufs-probe-4-check/`**.
Driver SHA256 **`52bb54cdfc90892dcda38ccff5d7071c81ec6a74d5ee210115dee68bb04096c5`**.

One-shot **`export_check.py` exits 0**, **13:54:37Z**: exact GNU-diff bytes,
reverse check against the prepared tree and isolated forward application
match; all **1108 protected hashes** are unchanged. Fresh full reconstruction
**exits 0**, **13:58:38–13:58:59Z**, **four staging scripts / 278 patches /
6369 source files exact**, all 15 historical baseline checks, only the two
documented dxb skips. Command/log/result records are in
**`research/starlte/ufs-probe-4-bringup/`**, reconstruction summary/log/inventory
in **`research/starlte/series-check-ufs-probe-4/`**. No new CI, image or phone
operation at this milestone; these software checks do not establish physical
Samsung acceptance or the cause of the prior timeout.

Continue reconciling every actual CI source file against independently fixed
expectations and prior-reviewed header pins. Root preparation still awaits
measured capacity/GPT and repeated userdata reads.

### Opt-in link startup and NOP candidate

The user prioritized the stalled UFS transport. Pinned Samsung
[ufshcd.c][samsung-ufshcd]:4040–4101,4180–4271 performs host enable, link startup
and device readiness checking. HCE/UCRDY and an unchanged OCS initializer do
not establish a working link or a fetched command descriptor. The candidate
on **`starlte/ufs-link-startup`**, based on **2347574**, fills that source gap:

1. Require **`ufsprobe=1 ufslinkinit=1`**, exact starlte identity, validated
   Image-owned DMA32 and the exact UniPro mapping. Keep both request lists
   stopped/idle while issuing UIC commands.
2. Apply Samsung's inherited-clock controls with masked read/modify/write and
   readback: clear FORCE_HCS core auto-gating bit `0x10`, clear CLKSTOP bits
   `0x17`, set ACG_DISABLE bit 0. Reapply after HCE initialization. The Samsung
   DT selects hw-rev 5; the revision-4 refclk mux path is not selected.
3. If this boot enabled HCE, permit missing Device Present during initialization,
   set local attributes **`0x3000=0`, `0x3001=1`, `0x4021=1`, `0x4020=1`**, then
   issue one **DME_LINKSTARTUP**. These CPort settings are from
   [ufs-cal-9810.c][samsung-cal]:153–156. Existing PHY calibration is assumed.
4. Poll each UIC command for at most **500 ms / 5000 iterations**, reject stale
   completion and failed acknowledgement, retain sampled IS errors, and check
   result/readiness/layer codes/list state. Samsung's direct UniPro result
   (`ufs-exynos.c:918–932`) is authoritative for LINKSTARTUP; generic ARG2 is
   also printed. Only an exact **IS error mask=4 / PA=`0x80000010` / other
   layers zero** during successfully completed LINKSTARTUP is consumed as its
   line-reset indication. That allowance never applies at transfer boundaries.
5. Verify connected TX/RX lanes (`0x1561`/`0x1581`, each 1–2), connected CPort
   (`0x4020=1`) and full HCS readiness. Already-enabled hosts receive these
   checks without LINKSTARTUP.
6. Configure the FMP-bypass request list, clear slot-0 SCSI Nexus, take a
   separate **pre-NOP** boundary, and require a valid **NOP OUT/IN (`0x00/0x20`)**
   with successful OCS and the correct 32-byte response. Only then allow SCSI.
   NOP uses the same bounded timeout/positive-bit cleanup and permanent
   quarantine as other transfers. Preserve 512-byte UPIU slots and the original
   pre-enable/pre-TUR sample order; the three stored boundary records print at
   the existing pre-root hold. A failed boundary submits no transfer.

The candidate command line adds `ufslinkinit=1` to the read-only probe arguments
above. Useful physical evidence is the **UIC command/result/setup** lines,
**warm link started/inherited**, **NOP IN result**, then TUR/capacity/GPT/read
results and **PROBE HOLD**. This sequence has not run on the phone yet.

Local validation **16:21:28–16:21:33Z** passed **329 host / 24 ADT / fatal
ASan/UBSan / complete real-header compilation**. Its model invalidates transport
on HCE independently of HCS: enable-only times out, while startup/NOP reaches
synthetic capacity/GPT/repeated reads. Tests also cover every UIC-stage timeout,
frozen counter, clock and mapping faults, direct-result disagreement,
acknowledgement faults, scoped LINERESET, malformed/missing NOP and late DMA.
Driver SHA256: **`0ca60b6862ecf62bdd90ede02cf04a654d7d840237b8f75ef999874799eabac7`**.
These are software results; the photographed timeout's cause remains unproven.

Patch 1136's exact export and isolated forward/reverse application passed
**16:26:57Z**. Initial reconstruction stopped at BearSSL DNS resolution
(`Could not resolve host: www.bearssl.org`) before applying any patches; its
tree/log/script remain in **`series-check-link-startup/`**. Fresh same-source
reconstruction passed **16:30:25Z**, **four stages / 279 patches / 6369 files
exact**, with only the two documented dxb skips. Candidate evidence lives under
this worktree's **`research/starlte/link-startup-check-1/`**, **`link-startup-export/`**
and **`series-check-link-startup-retry1/`**. Probe-4's main-checkout source and
fixed CI expectations remain pinned independently.

## Getting-started guide review for S9

The pasted guide describes Poco F1 / Qualcomm and older iOS18 defaults.
For this checkout and phone:

| Guide step | S9 / iOS26 equivalent |
| --- | --- |
| 0: Qualcomm appended DTBs | The supplied Samsung template has a separate DTBH section. Use the [Samsung packager](#samsung-bootimg-from-a-template). |
| 1: XNU11215 on `ry-diffusion/Bergamota` | XNU12377, patches 1131–1136, fork `friendlycatt/Bergamota`; dispatch with `macos_version=26.5`. Run **40826795487** predates UFS, **40931557218** predates the enable correction, **40955206538** predates pre-cleanup diagnostics, **41014066730** predates pre-submit boundaries. |
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

1. **Read-only Samsung storage.** Build and physically test the locally validated
   [startup/NOP candidate](#opt-in-link-startup-and-nop-candidate),
   using the supplied Samsung template and 512-byte UPIU layout. Obtain actual LUN
   capacity/GPT and repeated userdata reads before preparing the iOS26 root.
   A destructive userdata operation needs separate confirmation; no GPT changes.
2. **Extend the physical XNU record.** Probe 3's photograph and source review
   are complete: PA `0x80000010 -> 0`, pending DBR and initializer OCS before
   cleanup, then failure hold. Photograph startup/NOP results, boundary samples,
   any first-error/terminal samples, counters and hold (or last visible line).
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
[samsung-cal]: https://github.com/duhansysl/exynos9810-kernel/blob/914eabe7b6799ab0eb159360a5f9731dd50d0f1d/drivers/scsi/ufs/ufs-cal-9810.c
[samsung-upiu1024]: https://github.com/duhansysl/exynos9810-kernel/commit/a4bd396b7c2d46cf74e4eb1f9a8300f944efd8fb
[linux66-ufshci]: https://github.com/torvalds/linux/blob/ffc253263a1375a65fa6c9f62a893e9767fbebfa/include/ufs/ufshci.h
[linux66-ufshcd]: https://github.com/torvalds/linux/blob/ffc253263a1375a65fa6c9f62a893e9767fbebfa/drivers/ufs/core/ufshcd.c
[mu]: https://github.com/Project-Silicium/Mu-Silicium/tree/7a2ada19841a129f86fb791c0b9a710436c849a4
[mu-dts]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Resources/DTBs/starlte.dts
[mu-memory]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Platforms/Samsung/starltePkg/Library/MemoryMapLib/MemoryMapLib.c
[mu-board]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Platforms/Samsung/starltePkg/starlte.dsc
[mu-soc]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Silicon/Samsung/S5E9810Pkg/S5E9810Pkg.dsc.inc
[mu-sec]: https://github.com/Project-Silicium/Mu-Silicium/blob/7a2ada19841a129f86fb791c0b9a710436c849a4/Silicon/Samsung/S5E9810Pkg/Library/PlatformSecLib/PlatformSec.c
