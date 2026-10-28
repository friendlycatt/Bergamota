#!/usr/bin/env python3
"""Cross-compile the patched XNU interrupt path and exercise it on QEMU virt.

Requires a prepared XNU12377 source tree, clang/lld, llvm-objcopy/nm and QEMU.
No Apple SDK, full kernel image or physical device is used. See README.md.
"""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import adt
import mkadt

spec = importlib.util.spec_from_file_location('qemu_sweet', ROOT / 'tools/qemu-sweet.py')
qemu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qemu)


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, text=True, **kwargs)


def make_payload(tree, image, memory_size, version):
    """One freestanding ELF segment, packed using the documented SWEETXNU ABI."""
    blob = adt.serialize(tree)
    chosen = qemu.tree_node(tree, '/chosen')
    phys, = struct.unpack('<Q', chosen.prop('dram-base'))
    ram_size, = struct.unpack('<Q', chosen.prop('dram-size'))
    base = 0x50000000
    content = image
    if version == 2:
        compressor = zlib.compressobj(6, zlib.DEFLATED, -15)
        content = compressor.compress(image) + compressor.flush()
    offset = 0x400
    adt_offset = (offset + len(content) + 15) & ~15
    end = base + ((memory_size + 0x3fff) & ~0x3fff)
    header = struct.pack('<8sII7Q', b'SWEETXNU', version, 1, base, base,
                         adt_offset, len(blob), end, phys, ram_size)
    header += b'cpus=8 sweetearlyfb=1\0'.ljust(608, b'\0')
    header += struct.pack('<4Q', base, memory_size, offset, len(content))
    if version == 2:
        header += struct.pack('<Q', len(image))
    return header.ljust(offset, b'\0') + content + b'\0' * (adt_offset - offset - len(content)) + blob


def build_tree(**kwargs):
    with contextlib.redirect_stderr(io.StringIO()):
        return mkadt.build(seed=bytes(range(256)), **kwargs)


def host_checks(image, memory_size):
    tree = build_tree(board='starlte', qemu=True, qemu_cpus=8)
    one = make_payload(tree, image, memory_size, 1)
    two = make_payload(tree, image, memory_size, 2)
    assert qemu.build_image(one) == qemu.build_image(two), 'v1/v2 RAM differs'
    assert qemu.payload_data(two, 0x50000000, len(image)) == image
    assert qemu.qemu_config(two) == (2, 8)
    for length, tail in ((608, 728), (1024, 1144)):
        phys, ram, args, _ = qemu.build_image(two, length)
        assert struct.unpack_from('<Q', ram, args - phys + tail)[0] == 0x60000000
    # A truncated DEFLATE stream must be rejected even when output bytes match.
    bad = bytearray(two)
    size = struct.unpack_from('<Q', bad, 0x2a8 + 24)[0]
    struct.pack_into('<Q', bad, 0x2a8 + 24, size - 1)
    try:
        qemu.build_image(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('accepted truncated DEFLATE stream')
    hardware = build_tree(board='starlte')
    cpus = qemu.tree_node(hardware, '/cpus').children
    assert [struct.unpack('<I', c.prop('reg'))[0] for c in cpus] == [0, 1, 2, 3, 0x100, 0x101, 0x102, 0x103]
    assert qemu.tree_node(hardware, '/psci').prop('method') == b'smc\0'
    assert not any(c.name == 'sweet-ufs' for c in hardware.children)
    hp = make_payload(hardware, image, memory_size, 2)
    try:
        qemu.qemu_config(hp)
    except ValueError:
        pass
    else:
        raise AssertionError('accepted hardware MMIO addresses in QEMU')
    bad = bytearray(one)
    struct.pack_into('<Q', bad, 0x38, 0xcc000000)
    try:
        qemu.build_image(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('accepted framebuffer/RAM overlap')
    for board in ('sweet', 'beryllium', 'starlte'):
        for highmem in (False, True):
            for virt in (False, True):
                generated = build_tree(board=board, highmem=highmem, qemu=virt)
                qemu.build_image(make_payload(generated, image, memory_size, 2))
    print('PASS host: ADT memory/affinities, v1/v2 loading, boot_args ABIs, malformed payloads')


def compile_guest(xnu, out):
    include = out / 'include'
    for path in ('libkern/section_keywords.h', 'string.h', 'arm64/proc_reg.h',
                 'kern/clock.h', 'mach/mach_time.h', 'machine/atomic.h', 'machine/machine_routines.h',
                 'pexpert/device_tree.h', 'pexpert/arm64/board_config.h', 'kern/misc_protos.h', 'pexpert/pexpert.h'):
        dest = include / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text('/* XNU service declarations are supplied by compat.h. */\n')
    flags = ['-O2', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-mgeneral-regs-only',
             '-mno-outline-atomics', '-Wall', '-Wextra', '-Werror', '-DARM64_BOARD_CONFIG_SWEET=1',
             '-include', HERE / 'compat.h', '-include', xnu / 'pexpert/pexpert/arm64/SWEET.h',
             '-I', include, '-I', xnu / 'pexpert', '-I', xnu / 'osfmk']
    sources = [xnu / 'pexpert/arm/pe_fiq.c', xnu / 'osfmk/arm64/sweet_early.c', HERE / 'guest.c']
    objects = []
    for source in sources:
        obj = out / (source.stem + '.o')
        run(['clang', '--target=aarch64-none-elf', *flags, '-c', source, '-o', obj])
        objects.append(obj)
    # Also compile the driver/console with the Mach-O target used by XNU.
    for source in sources[:2]:
        run(['clang', '--target=arm64-apple-macos26', *flags, '-c', source, '-o', out / (source.stem + '.macho.o')])
    run(['clang', '--target=aarch64-none-elf', '-c', HERE / 'start.S', '-o', out / 'start.o'])
    elf = out / 'guest.elf'
    run(['ld.lld', '-T', HERE / 'link.ld', out / 'start.o', *objects, '-o', elf])
    run(['llvm-objcopy', '-O', 'binary', elf, out / 'guest.bin'])
    symbols = run(['llvm-nm', '--defined-only', elf], capture_output=True).stdout
    end = next(int(s.split()[0], 16) for s in symbols.splitlines() if s.split()[-1] == 'test_end')
    print('PASS cross-compile: production pe_fiq.c + sweet_early.c, ELF and Mach-O')
    return (out / 'guest.bin').read_bytes(), end - 0x50000000


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu', type=Path, default=ROOT / 'research/starlte/xnu')
    ap.add_argument('--out', type=Path, default=ROOT / 'research/starlte/check')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image, memory_size = compile_guest(args.xnu.resolve(), out)
    host_checks(image, memory_size)
    results = []
    for gic, cpus, version in ((2, 1, 1), (2, 8, 2), (3, 1, 2), (3, 8, 1)):
        case = out / f'gic{gic}-{cpus}cpu-v{version}'
        case.mkdir(exist_ok=True)
        tree = build_tree(board='starlte', qemu=True, gic_version=gic, qemu_cpus=cpus)
        payload = case / 'payload.bin'
        payload.write_bytes(make_payload(tree, image, memory_size, version))
        with (case / 'runner.log').open('w') as log:
            run([sys.executable, ROOT / 'tools/qemu-sweet.py', payload, case, '--secs', '2'], stdout=log, stderr=subprocess.STDOUT)
        serial = (case / 'serial.txt').read_text()
        expected = f'PASS GICv{gic} CPUs={cpus} timers=32/cpu SGIs={cpus + cpus * cpus * 3}'
        if expected not in serial or 'FAIL:' in serial:
            raise SystemExit(f'{case}: guest failed\n{serial}')
        png = (case / 'fb.png').read_bytes()
        assert struct.unpack_from('>II', png, 16) == (1440, 2960)
        registers = (case / 'regs.txt').read_text().splitlines()
        assert [line for line in registers if line.startswith('CPU#')] == [f'CPU#{cpu}' for cpu in range(cpus)]
        result = dict(gic=gic, cpus=cpus, payload_version=version, status='pass', timers=32 * cpus, sgis=cpus + cpus * cpus * 3)
        results.append(result)
        print(expected)
    (out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
