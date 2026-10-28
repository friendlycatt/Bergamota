#!/usr/bin/env python3
"""Execute XNU's production reset assembly and inspect its 16K page tables.

Uses real boot.h / architectural headers, complete production genassym output,
and test-owned C-service replacements. This is not a full kernel boot.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

from check import ROOT, build_tree, qemu, run
import adt
import mkadt
import mkpayload
import headers

HERE = Path(__file__).resolve().parent
BASE = 0xfffffff008000000
TABLE_SIZE = 0x20000
BOOT_FLAGS = 0x1234567800000001


def macho_section(path, segment, section):
    raw = path.read_bytes()
    offset = 32
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        cmd, size = struct.unpack_from('<II', raw, offset)
        if cmd == 0x19:
            for i in range(struct.unpack_from('<I', raw, offset + 64)[0]):
                start = offset + 72 + 80 * i
                sect, seg = (raw[start + n:start + n + 16].split(b'\0')[0].decode() for n in (0, 16))
                if (seg, sect) == (segment, section):
                    length, fileoff = struct.unpack_from('<QI', raw, start + 40)
                    return raw[fileoff:fileoff + length]
        offset += size
    raise ValueError(f'{path}: missing {segment},{section}')


def flags(xnu, include):
    return ['--target=arm64-apple-macos26', '-ffreestanding',
            '-DKERNEL_PRIVATE=1', '-DXNU_KERNEL_PRIVATE=1', '-DMACH_KERNEL_PRIVATE=1',
            '-DARM64_BOARD_CONFIG_SWEET=1', '-DDEVELOPMENT=1',
            '-I', include, '-I', xnu / 'osfmk', '-I', xnu / 'pexpert',
            '-I', xnu / 'bsd', '-I', xnu / 'EXTERNAL_HEADERS']


def layout(xnu, out, configuration):
    """Compile full genassym plus boot register/structure-size expressions."""
    result = headers.compile_source(xnu, configuration, 'osfmk/arm64/genassym.c', out / 'genassym.s')
    (out / 'genassym-command.json').write_text(json.dumps(result, indent=2) + '\n')
    assert not result['returncode'], 'run headers.py to generate the XNU configuration first'
    production = headers.genassym_values(out / 'genassym.s')
    include = out / 'include'
    include.mkdir(exist_ok=True)
    (include / 'mach_assert.h').write_text('#define MACH_ASSERT 1\n')
    declarations = re.findall(r'^\s*DECLARE\("BA_[^\n]+;',
                              (xnu / 'osfmk/arm64/genassym.c').read_text(), re.M)
    assert len(declarations) == 9, 'review changed boot_args declarations in genassym.c'
    source = out / 'boot-layout.c'
    source.write_text('''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mach/boolean.h>
#include <mach/vm_types.h>
#include <pexpert/arm64/boot.h>
#include <arm64/proc_reg.h>
#define DECLARE(SYM, VAL) __asm("LAYOUT_" SYM ": .ascii \\"%0\\"" : : "i" ((unsigned long)(VAL)))
void boot_layout(void) {
''' + '\n'.join(declarations) + '''
    DECLARE("BA_SIZE", sizeof(struct boot_args));
    DECLARE("PGBYTES", ARM_PGBYTES);
    DECLARE("TCR", TCR_EL1_BOOT);
    DECLARE("SCTLR", SCTLR_EL1_DEFAULT);
}
''')
    # boot_args has the same layout in public/private kernel headers. Public
    # kernel mode avoids unrelated generated IPC/scheduler headers here.
    run(['clang', *flags(xnu, include), '-DKERNEL=1', '-UXNU_KERNEL_PRIVATE', '-UMACH_KERNEL_PRIVATE',
         '-I', xnu / 'libkern', '-Wno-visibility', '-fno-integrated-as',
         '-S', source, '-o', out / 'boot-layout.s'])
    values = {name: int(value, 0) for name, value in re.findall(
        r'LAYOUT_(\w+):\s*\.ascii\s+"#?(0x[0-9a-fA-F]+|[0-9]+)"',
        (out / 'boot-layout.s').read_text())}
    assert len(values) == 13 and values['BA_SIZE'] == 1152 and values['PGBYTES'] == qemu.PAGE, values
    (out / 'boot-layout.json').write_text(json.dumps(values, indent=2) + '\n')
    for name, value in values.items():
        if name in production:
            assert value == production[name], (name, value, production[name])
    values = {**production, **values}
    definitions = {**values, **{name + '_NUM': value for name, value in production.items()},
                   'VM_KERNEL_LINK_ADDRESS': BASE, 'TEST_CPU_COUNT': 8}
    (include / 'assym.s').write_text(
        '/* Full production genassym, with boot.h/register-derived fixture constants. */\n' +
        ''.join(f'#define {name} {value:#x}\n' for name, value in definitions.items()))
    (out / 'layout.json').write_text(json.dumps(values, indent=2) + '\n')
    return values


def compile_reset(xnu, out, configuration, final_fb=False):
    values = layout(xnu, out, configuration)
    objects = [out / 'start.o', out / 'support.o']
    for source, name in ((xnu / 'osfmk/arm64/start.s', 'start'), (HERE / 'reset_support.S', 'support')):
        run(['clang', *flags(xnu, out / 'include'), '-x', 'assembler-with-cpp', '-DASSEMBLER=1',
             f'-DTEST_FINAL_FB={int(final_fb)}',
             '-c', source, '-o', out / f'{name}.o'])
    if final_fb:
        for source, name in ((HERE / 'final_framebuffer.c', 'final'),
                             (xnu / 'osfmk/arm64/sweet_early.c', 'early')):
            wrapper = out / f'{name}-wrapper.c'
            # -mkernel normally groups functions for the full kernel linker.
            # Allow this fixture to dead-strip uncalled kernel services while
            # retaining every instruction and real assertion in the callees.
            wrapper.write_text(f'#include "{source}"\n__asm__(".subsections_via_symbols");\n')
            command = ['clang', *headers.flags(xnu, configuration, 'osfmk'),
                       '-std=gnu17', '-O2', '-mgeneral-regs-only', '-fno-stack-protector', '-c', str(wrapper),
                       '-o', str(out / f'{name}.o')]
            (out / f'{name}-command.json').write_text(json.dumps(command, indent=2) + '\n')
            with (out / f'{name}.compile.log').open('w') as log:
                run(command, stdout=log, stderr=subprocess.STDOUT)
            # Static-if's live-support section pins every function through the
            # shared assertion key. This guest never patches static branches;
            # remove only that metadata so unrelated VM services can be stripped.
            original, obj = out / f'{name}.o', out / f'{name}-gc.o'
            run(['llvm-objcopy', '--remove-section=__BOOTDATA,__static_if', original, obj])
            assert macho_section(original, '__TEXT', '__text') == macho_section(obj, '__TEXT', '__text')
            objects.append(obj)
    linked = out / 'reset.linked'
    run(['ld64.lld', '-arch', 'arm64', '-platform_version', 'macos', '26.0', '26.0',
         '-e', '__start', '-pagezero_size', '0', '-headerpad', '0x400',
         '-no_uuid', '-no_adhoc_codesign', *(['-dead_strip'] if final_fb else []),
         *objects, '-o', linked])
    symbols = {s.split()[2]: int(s.split()[0], 16) for s in
               run(['llvm-nm', '--defined-only', linked], capture_output=True).stdout.splitlines()
               if len(s.split()) == 3}
    (out / 'symbols.json').write_text(json.dumps(symbols, indent=2) + '\n')
    raw = bytearray(linked.read_bytes())
    info = mkpayload.parse_macho(raw)
    assert info['segs'][0]['vmaddr'] == 0 and info['segs'][0]['fileoff'] == 0
    # ld64.lld lacks -image_base / LC_UNIXTHREAD output. The reset-only fixture
    # is PC-relative; C assertion descriptors also need image-relative rebases.
    assert info['fixups']
    rebases = mkpayload.apply_chained_fixups(bytearray(raw), info)
    if final_fb:
        dataoff = info['fixups'][0]
        starts = dataoff + struct.unpack_from('<I', raw, dataoff + 4)[0]
        count, = struct.unpack_from('<I', raw, starts)
        for i in range(count):
            offset, = struct.unpack_from('<I', raw, starts + 4 + 4 * i)
            if offset:
                table = starts + offset
                _, page_size, fmt, _, _, pages = struct.unpack_from('<IHHQIH', raw, table)
                assert fmt in (2, 6), f'unexpected fixture chain format {fmt}'
                for page in range(pages):
                    start, = struct.unpack_from('<H', raw, table + 22 + page * 2)
                    if start == 0xffff:
                        continue
                    assert not start & 0x8000, 'multi-start fixture chain'
                    loc = info['segs'][i]['fileoff'] + page * page_size + start
                    while True:
                        pointer, = struct.unpack_from('<Q', raw, loc)
                        target, nxt, stride = mkpayload.decode(pointer, fmt, 0)
                        assert target < (1 << 36) and any(
                            s['vmaddr'] <= target < s['vmaddr'] + s['vmsize'] for s in info['segs'])
                        if not nxt:
                            break
                        loc += nxt * stride
                # At link base zero these formats encode identical offsets.
                # Make the relative intent explicit before shifting the VAs;
                # mkpayload then rebases every descriptor to the new image base.
                struct.pack_into('<H', raw, table + 6, 6)
    else:
        assert rebases == 0, rebases
    ncmds, old_size = struct.unpack_from('<II', raw, 16)
    offset, commands, first_section = 32, [], len(raw)
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', raw, offset)
        part = bytearray(raw[offset:offset + size])
        if cmd == 0x19:  # LC_SEGMENT_64
            addr, = struct.unpack_from('<Q', part, 24)
            struct.pack_into('<Q', part, 24, addr + BASE)
            nsects, = struct.unpack_from('<I', part, 64)
            for i in range(nsects):
                section = 72 + i * 80
                addr, = struct.unpack_from('<Q', part, section + 32)
                struct.pack_into('<Q', part, section + 32, addr + BASE)
                fileoff, = struct.unpack_from('<I', part, section + 48)
                if fileoff:
                    first_section = min(first_section, fileoff)
        elif cmd == 0x2:  # LC_SYMTAB: keep the inspection symbols accurate too.
            symoff, count = struct.unpack_from('<II', part, 8)
            for i in range(count):
                entry = symoff + i * 16
                if raw[entry + 4] & 0x0e == 0x0e:  # N_SECT
                    addr, = struct.unpack_from('<Q', raw, entry + 8)
                    struct.pack_into('<Q', raw, entry + 8, addr + BASE)
        if cmd != 0x80000028:  # Replace LC_MAIN with LC_UNIXTHREAD.
            commands.append(part)
        offset += size
    regs = [0] * 34
    regs[32] = BASE + symbols['__start']
    commands.append(struct.pack('<4I34Q', 5, 288, 6, 68, *regs))
    command_bytes = b''.join(commands)
    assert 32 + max(old_size, len(command_bytes)) <= first_section
    raw[32:32 + len(command_bytes)] = command_bytes
    struct.pack_into('<II', raw, 16, len(commands), len(command_bytes))
    kernel = out / 'reset.macho'
    kernel.write_bytes(raw)
    assert mkpayload.parse_macho(raw)['entry'] == BASE + symbols['__start']
    (out / 'fixture.json').write_text(json.dumps(dict(final_framebuffer=final_fb, rebases=rebases), indent=2) + '\n')
    print(f'PASS reset compile: full production start.s/genassym, {rebases} image-relative rebases')
    return kernel, symbols, values


def set_prop(node, name, value):
    node.props = [(key, flags, value if key == name else old) for key, flags, old in node.props]


def execute_reset(case, blob, symbols, values, final_fb=False):
    (case / 'result.json').unlink(missing_ok=True)
    phys, image, args, entry = qemu.build_image(blob)
    image = bytearray(image)
    struct.pack_into('<Q', image, args - phys + values['BA_BOOT_FLAGS'], BOOT_FLAGS)
    tables = symbols['_bootstrap_pagetables']
    image[tables:tables + TABLE_SIZE] = b'\xa5' * TABLE_SIZE
    (case / 'ram.bin').write_bytes(image)
    (case / 'stub.bin').write_bytes(qemu.stub(args, entry))
    fb = qemu.framebuffer(blob)
    memsize, = struct.unpack_from('<Q', blob, 0x40)
    ram_gib = max(4, -(-(max(phys + memsize, fb.base + fb.size) - 0x40000000) // (1 << 30)))
    gic, cpus = qemu.qemu_config(blob)
    sock = case / 'qmp.sock'
    sock.unlink(missing_ok=True)
    cmd = ['qemu-system-aarch64', '-M', f'virt,gic-version={gic}', '-cpu', 'cortex-a76',
           '-m', f'{ram_gib}G', '-smp', str(cpus), '-nographic', '-serial', f'file:{case / "serial.txt"}',
           '-qmp', f'unix:{sock},server=on,wait=off', '-no-reboot',
           '-d', 'int,guest_errors', '-D', str(case / 'qemu.log'),
           '-device', f'loader,file={case / "ram.bin"},addr={phys:#x},force-raw=on',
           '-device', f'loader,file={case / "stub.bin"},addr={qemu.STUB_PA:#x},force-raw=on',
           '-device', f'loader,addr={qemu.STUB_PA:#x},cpu-num=0']
    (case / 'command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    with (case / 'runner.log').open('w') as log:
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        try:
            time.sleep(1)
            assert proc.poll() is None, f'{case}: QEMU exited ({proc.returncode})'
            replies = qemu.qmp(str(sock), 'stop', 'info registers',
                               pmemsave=(phys + symbols['_test_report'], 80, str(case / 'report.bin')))
            (case / 'regs.txt').write_text(replies[-1])
            qemu.qmp(str(sock), pmemsave=(phys + tables, TABLE_SIZE, str(case / 'tables.bin')))
            qemu.qmp(str(sock), pmemsave=(fb.base, fb.size, str(case / 'fb.raw')))
            if final_fb:
                for symbol, size, name in (('_test_final_report', 96, 'final-report'),
                                           ('_test_kernel_root', qemu.PAGE, 'final-root'),
                                           ('_test_fb_l2', qemu.PAGE, 'final-l2'),
                                           ('_test_invalid_root', qemu.PAGE, 'invalid-root')):
                    qemu.qmp(str(sock), pmemsave=(phys + symbols[symbol], size, str(case / f'{name}.bin')))
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            sock.unlink(missing_ok=True)
    report = struct.unpack('<10Q', (case / 'report.bin').read_bytes())
    assert report == (0x5253, BASE + args - phys, 4, values['SCTLR'], values['TCR'],
                      phys + tables, phys + tables + qemu.PAGE, fb.base, BOOT_FLAGS, 0x5041), report
    pc = int(re.search(r'PC=([0-9a-f]+)', (case / 'regs.txt').read_text()).group(1), 16)
    assert pc in (BASE + symbols['_test_done'], BASE + symbols['_test_done'] + 4), hex(pc)
    assert not (case / 'qemu.log').read_text(), f'{case}: unexpected QEMU exception/MMIO error'
    raw = (case / 'fb.raw').read_bytes()
    if final_fb:
        final_result = check_final_framebuffer(case, fb, symbols, values, phys, raw)
    else:
        assert struct.unpack_from('<I', raw)[0] == 0x11223344
        assert struct.unpack_from('<I', raw, len(raw) - 4)[0] == 0x55667788
        for y in range(fb.height - 32, fb.height):
            start = y * fb.stride
            assert raw[start:start + 64 * 4] == b'\xff' * (64 * 4), f'wrong stage-0 marker row {y}'
            assert raw[start + 64 * 4:start + 65 * 4] == b'\0' * 4
    qemu.write_png(case / 'fb.png', raw, fb)
    (case / 'fb.raw').unlink()
    data = (case / 'tables.bin').read_bytes()

    def word(pa):
        offset = pa - phys - tables
        assert 0 <= offset <= TABLE_SIZE - 8, f'table outside bootstrap allocation: {pa:#x}'
        return struct.unpack_from('<Q', data, offset)[0]

    def walk(root, va):
        l1 = word(root + ((va >> 36) & 0x7ff) * 8)
        assert l1 & 3 == 3, f'L1 invalid for {va:#x}: {l1:#x}'
        l2pa = l1 & 0x0000ffffffffc000
        l2 = word(l2pa + ((va >> 25) & 0x7ff) * 8)
        assert l2 & 3 == 1, f'L2 block invalid for {va:#x}: {l2:#x}'
        return (l2 & 0x0000fffffe000000) | (va & 0x1ffffff), l2pa, l2

    for va in (BASE + symbols['__start'], BASE + args - phys):
        assert walk(report[6], va)[0] == phys + va - BASE
    assert walk(report[5], entry)[0] == entry
    mapped, fb_l2, descriptor = walk(report[5], fb.base)
    assert mapped == fb.base and descriptor & 0xfff == 0x605  # NC, outer-shareable, AF
    assert descriptor >> 53 & 3 == 3  # UXN + PXN
    separate = phys >> 36 != fb.base >> 36
    if separate:
        assert fb_l2 != walk(report[5], entry)[1]
        # A new table must have exactly one valid block, despite initially dirty RAM.
        entries = [word(fb_l2 + i * 8) for i in range(2048)]
        assert sum(value != 0 for value in entries) == 1
    result = dict(status='pass', phys_base=hex(phys), framebuffer=hex(fb.base),
                  ram_size=hex(memsize), separate_l1=separate, boot_args=hex(report[1]),
                  ttbr0=hex(report[5]), ttbr1=hex(report[6]), exceptions=0)
    if final_fb:
        result.update(final_result)
    (case / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    detail = 'final framebuffer alias, TTBR0 removed' if final_fb else 'KVA handoff, framebuffer endpoints/marker'
    print(f'PASS reset {case.name}: {detail}, 16K tables, no exceptions')
    return result


def check_final_framebuffer(case, fb, symbols, values, phys, pixels):
    report = struct.unpack('<12Q', (case / 'final-report.bin').read_bytes())
    va = (values['VM_MIN_KERNEL_ADDRESS'] + 0x2000000) & ((1 << 64) - 1)
    block = fb.base & ~0x1ffffff
    alias = va + (fb.base - block)
    assert report[:3] == (0x4642, va, block), report
    assert report[3] & 1, 'outgoing framebuffer VA still translates after TTBR0 teardown'
    for par, pa in ((report[4], fb.base), (report[5], fb.base + fb.size - 4)):
        assert not par & 1 and par & 0x0000ffffffffc000 == pa & ~0x3fff, (hex(par), hex(pa))
    assert report[6:8] == (phys + symbols['_test_invalid_root'], phys + symbols['_test_kernel_root']), report
    assert report[9:] == (alias, fb.size, 0x5041), report
    assert (case / 'invalid-root.bin').read_bytes() == bytes(qemu.PAGE)
    root = (case / 'final-root.bin').read_bytes()
    l1, = struct.unpack_from('<Q', root, ((va >> 36) & 0x7ff) * 8)
    assert l1 == phys + symbols['_test_fb_l2'] | 3, hex(l1)
    l2 = struct.unpack('<2048Q', (case / 'final-l2.bin').read_bytes())
    descriptor = l2[(va >> 25) & 0x7ff]
    assert descriptor == report[8] == block | (3 << 53) | 0x605, hex(descriptor)
    assert sum(entry != 0 for entry in l2) == 1
    assert struct.unpack_from('<I', pixels)[0] == 0x89abcdef
    assert struct.unpack_from('<I', pixels, len(pixels) - 4)[0] == 0xfedcba98
    # Relocate writes stages 8/9 before TTBR0 removal; stage 10 proves the
    # production console retains its alias after the TLB is flushed.
    for stage in (8, 9, 10):
        color = struct.pack('<I', 0xff00ff00 | ((stage & 7) * 32 << 16))
        for y in range(fb.height - 32, fb.height):
            start = y * fb.stride + stage * 48 * 4
            assert pixels[start:start + 48 * 4] == color * 48, (stage, y)
    return dict(final_alias=hex(alias), final_ttbr0=hex(report[6]), final_ttbr1=hex(report[7]),
                outgoing_alias_fault=True, final_framebuffer='pass')


def execute_warm(case, cold, symbols, values, gic, el):
    """Preload test-owned CPU records/root, then execute all production warm paths.

    GICv2/v3 and EL1/EL2 are independently exercised. The EL2 fixture sets
    VMPIDR_EL2 to Exynos-shaped affinities; QEMU's real CPU topology is unaltered.
    A test-owned PSCI caller wakes QEMU's powered-off CPUs. Samsung firmware
    and IOKit registration are not part of this fixture.
    """
    (case / 'result.json').unlink(missing_ok=True)
    blob = (cold / 'payload.bin').read_bytes()
    phys, _, args, _ = qemu.build_image(blob)
    image = bytearray((cold / 'ram.bin').read_bytes())
    tables = (cold / 'tables.bin').read_bytes()
    bootstrap = symbols['_bootstrap_pagetables']
    root = symbols['_test_kernel_root']
    image[bootstrap:bootstrap + len(tables)] = tables
    image[root:root + qemu.PAGE] = tables[qemu.PAGE:2 * qemu.PAGE]
    const_args = symbols['_const_boot_args']
    image[const_args:const_args + values['BA_SIZE']] = image[args - phys:args - phys + values['BA_SIZE']]

    def word(offset, value):
        struct.pack_into('<Q', image, offset, value)

    word(symbols['_cpu_ttep'], phys + root)
    word(symbols['_ResetHandlerData'] + values['CPU_DATA_ENTRIES'], phys + symbols['_test_cpu_entries'])
    cpus = 8
    expected = []
    for cpu in range(cpus):
        logical = (cpu * 3) % cpus  # Disagree with both MPIDR and table scan order.
        mpidr = (cpu & 3) | ((cpu >> 2) << 8) if el == 2 else cpu
        data = symbols['_test_cpu_data'] + logical * values['cdSize']
        entry = symbols['_test_cpu_entries'] + logical * values['cdeSize']
        word(entry + values['CPU_DATA_PADDR'], phys + data)
        word(entry + values['CPU_DATA_VADDR'], BASE + data)
        struct.pack_into('<I', image, data + values['CPU_NUMBER_GS'], logical)
        struct.pack_into('<I', image, data + values['CPU_PHYS_ID'], mpidr)
        handler = '_resume_idle_cpu' if cpu & 1 else '_start_cpu'
        word(data + values['CPU_RESET_HANDLER'], phys + symbols[handler])
        tpidr = 0xface00000000 | logical
        word(data + values['CPU_TPIDR_EL0'], tpidr)
        excepstack = BASE + symbols['_test_cpu_stacks'] + (logical * 2 + 1) * qemu.PAGE
        intstack = excepstack + qemu.PAGE
        word(data + values['CPU_EXCEPSTACK_TOP'], excepstack)
        word(data + values['CPU_INTSTACK_TOP'], intstack)
        expected.append((logical, (0x4944 if cpu & 1 else 0x5354, BASE + data,
                                   4, values['SCTLR'], values['TCR'], phys + bootstrap,
                                   phys + root, tpidr, intstack, excepstack,
                                   BASE + symbols['_ExceptionVectorsBase'], 0, mpidr,
                                   BOOT_FLAGS, 0, 0x5041)))
    (case / 'ram.bin').write_bytes(image)
    entry = symbols['_test_warm_boot_entry']
    (case / 'stub.bin').write_bytes(qemu.stub(0xbadc0de, phys + entry))
    sock = case / 'qmp.sock'
    sock.unlink(missing_ok=True)
    cmd = ['qemu-system-aarch64', '-M', f'virt,gic-version={gic},virtualization={"on" if el == 2 else "off"}',
           '-cpu', 'cortex-a76', '-m', '4G', '-smp', str(cpus), '-nographic',
           '-serial', f'file:{case / "serial.txt"}', '-qmp', f'unix:{sock},server=on,wait=off',
           '-no-reboot', '-d', 'int,guest_errors', '-D', str(case / 'qemu.log'),
           '-device', f'loader,file={case / "ram.bin"},addr={phys:#x},force-raw=on',
           '-device', f'loader,file={case / "stub.bin"},addr={qemu.STUB_PA:#x},force-raw=on']
    cmd += ['-device', f'loader,addr={qemu.STUB_PA:#x},cpu-num=0']
    (case / 'command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    with (case / 'runner.log').open('w') as log:
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        try:
            time.sleep(1)
            assert proc.poll() is None, f'{case}: QEMU exited ({proc.returncode})'
            qemu.qmp(str(sock), 'stop', pmemsave=(phys + symbols['_test_warm_reports'], cpus * 128,
                                                str(case / 'reports.bin')))
            qemu.qmp(str(sock), pmemsave=(phys + symbols['_test_psci_results'], cpus * 8,
                                        str(case / 'psci.bin')))
            registers = []
            for cpu in range(cpus):
                text = qemu.qmp(str(sock), 'info registers', cpu_index=cpu)[-1]
                registers.append(text)
                (case / f'cpu{cpu}-regs.txt').write_text(text)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            sock.unlink(missing_ok=True)
    reports = (case / 'reports.bin').read_bytes()
    psci = struct.unpack('<8Q', (case / 'psci.bin').read_bytes())
    assert psci == (0x5041, *([0] * (cpus - 1))), psci
    for cpu, (logical, wanted) in enumerate(expected):
        report = struct.unpack_from('<16Q', reports, logical * 128)
        assert report == wanted, (case.name, cpu, logical, report, wanted)
        assert f'CPU#{cpu}' in registers[cpu].splitlines(), registers[cpu]
        pc = int(re.search(r'PC=([0-9a-f]+)', registers[cpu]).group(1), 16)
        assert pc in (BASE + symbols['_test_warm_done'], BASE + symbols['_test_warm_done'] + 4), hex(pc)
    trace = (case / 'qemu.log').read_text().splitlines()
    call = ('Taking exception 11 [Hypervisor Call] on CPU 0' if el == 1 else
            'Taking exception 13 [Secure Monitor Call] on CPU 0')
    allowed = {call, f'...from EL{el} to EL{el + 1}',
               '...with ESR 0x16/0x5a000000' if el == 1 else '...with ESR 0x17/0x5e000000',
               '...handled as PSCI call'}
    returns = [line for line in trace if re.fullmatch(
        r'Exception return from AArch64 EL2 to AArch64 EL1(?: PC 0x[0-9a-f]+)?', line)]
    assert all(line in allowed or line in returns for line in trace), trace
    assert trace.count(call) == cpus - 1, trace
    assert trace.count('...handled as PSCI call') == cpus - 1, trace
    assert len(returns) == (cpus if el == 2 else 0), trace
    result = dict(kind='warm', status='pass', gic=gic, entry_el=el, cpus=cpus,
                  start_cpu=4, resume_idle_cpu=4, affinities=[hex(row[1][12]) for row in expected],
                  affinity_source='VMPIDR_EL2 fixture' if el == 2 else 'QEMU MPIDR_EL1',
                  psci_calls=cpus - 1, unexpected_exceptions=0)
    (case / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS warm GICv{gic} EL{el}: 8 CPUs, start/idle handoffs, real CPU offsets/stacks, no unexpected exceptions')
    return result


def suite(xnu, out, configuration):
    kernel, symbols, values = compile_reset(xnu, out, configuration)
    cases = [('starlte-qemu', 'starlte', None, 1),
             ('starlte-upper-bank', 'starlte', mkadt.BOARDS['starlte']['mem'], 2),
             ('sweet-framebuffer', 'sweet', None, 2),
             ('beryllium-framebuffer', 'beryllium', None, 1),
             ('separate-l1', 'starlte', (0x1080000000, 0x80000000), 2)]
    for name, board, memory, version in cases:
        case = out / name
        case.mkdir(exist_ok=True)
        tree = build_tree(board=board, qemu=True)
        if memory:
            chosen = qemu.tree_node(tree, '/chosen')
            set_prop(chosen, 'dram-base', struct.pack('<Q', memory[0]))
            set_prop(chosen, 'dram-size', struct.pack('<Q', memory[1]))
        path = case / 'fixture.adt'
        path.write_bytes(adt.serialize(tree))
        blob, _ = mkpayload.build(kernel, path, 'debug=0xc cpus=1', compress=version == 2)
        (case / 'payload.bin').write_bytes(blob)
        yield dict(kind='cold', case=name, payload_version=version,
                   **execute_reset(case, blob, symbols, values))
    for gic in (2, 3):
        for el in (1, 2):
            case = out / f'warm-gic{gic}-el{el}'
            case.mkdir(exist_ok=True)
            yield dict(case=case.name, **execute_warm(case, out / 'starlte-qemu', symbols, values, gic, el))
    final = out / 'final-fixture'
    final.mkdir(exist_ok=True)
    kernel, symbols, values = compile_reset(xnu, final, configuration, final_fb=True)
    for board in ('starlte', 'sweet', 'beryllium'):
        case = final / board
        case.mkdir(exist_ok=True)
        path = case / 'fixture.adt'
        path.write_bytes(adt.serialize(build_tree(board=board, qemu=True)))
        blob, _ = mkpayload.build(kernel, path, 'debug=0xc cpus=1')
        (case / 'payload.bin').write_bytes(blob)
        yield dict(kind='final-framebuffer', case=board,
                   **execute_reset(case, blob, symbols, values, final_fb=True))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu', type=Path, default=ROOT / 'research/starlte/xnu')
    ap.add_argument('--out', type=Path, default=ROOT / 'research/starlte/reset-check')
    ap.add_argument('--headers', type=Path, default=ROOT / 'research/starlte/header-check',
                    help='configuration generated by headers.py')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = []

    def save(status, error=None):
        (out / 'summary.json').write_text(json.dumps(
            dict(status=status, expected_cases=12, results=results, error=error), indent=2) + '\n')

    save('running')  # A failed rerun must never leave an older PASS summary.
    try:
        for result in suite(args.xnu.resolve(), out, args.headers.resolve()):
            results.append(result)
            save('running')
    except BaseException as error:
        save('failed', f'{type(error).__name__}: {error}')
        raise
    save('pass')


if __name__ == '__main__':
    main()
