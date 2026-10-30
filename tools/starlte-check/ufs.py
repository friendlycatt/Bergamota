#!/usr/bin/env python3
"""Exercise production UFS transport/discovery with a byte-level host emulator.

The emulator owns MMIO, SMC, time, DMA mapping and BSD services. Production
transport, GPT, initialization and strategy functions are compiled unchanged;
the complete translation unit is separately compiled with real XNU headers.
See README.md for the precise service/extraction boundary.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib

from check import ROOT, build_tree, run
import adt
from enxerto import fill_dma
import headers

HERE = Path(__file__).resolve().parent


def function(source, name):
    match = re.search(r'^(?:static (?:inline )?)?[\w *]+\n' + re.escape(name) +
                      r'\([^;]*?\)\n\{.*?^\}\n', source, re.M | re.S)
    assert match, name
    return match[0]


def host_source(xnu, out):
    source = (xnu / 'bsd/dev/sweet_ufs.c').read_text()
    core = source[source.index('#define REG_CAP'):source.index('static int\nsweet_ufs_open')]
    replacements = {
        'ufs_rd': 'return model_rd(off, false);',
        'ufs_wr': 'model_wr(off, v, false);',
        'exynos_rd': 'return model_rd(off, true);',
        'exynos_wr': 'model_wr(off, v, true);',
        'exynos_unipro_rd': 'return model_unipro_rd(off);',
        'exynos_fmp_format': 'return model_smc();',
    }
    boundary = {}
    for name, body in replacements.items():
        old = function(core, name)
        boundary[name] = hashlib.sha256(old.encode()).hexdigest()
        new = old[:old.index('\n{')] + '\n{\n\t' + body + '\n}\n'
        assert core.count(old) == 1
        core = core.replace(old, new)
    names = ('sweet_ufs_open', 'sweet_ufs_strategy', 'sweet_ufs_ioctl',
             'ufs_dt_read', 'exynos_dma_handoff', 'ufs_probe_hold', 'sweet_ufs_init', 'sweet_ufs_rootdev')
    selected = {name: function(source, name) for name in names}
    aux = source[source.index('#define PART_LUNS'):source.index('static bool\nufs_dt_read')]
    generated = core + '\n' + aux + '\n' + '\n'.join(selected.values())
    (out / 'ufs-production.inc').write_text(generated)
    return dict(source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                replaced_hardware_services=boundary,
                unchanged_functions={name: hashlib.sha256(text.encode()).hexdigest()
                                     for name, text in selected.items()},
                generated_sha256=hashlib.sha256(generated.encode()).hexdigest())


def fixture(out, block):
    """Independent GPT producer: fixed public synthetic data, CRCs from zlib."""
    entries = bytearray(8192)
    entries[:16] = bytes.fromhex('a2a0d0ebe5b9334487c068b6b72699c7')
    entries[16:32] = bytes(range(16))
    struct.pack_into('<QQ', entries, 32, 0x100, 0x20ff)
    label = 'USERDATA'.encode('utf-16le')
    entries[56:56 + len(label)] = label
    header = bytearray(block)
    header[:8] = b'EFI PART'
    struct.pack_into('<IIIIQQQQ', header, 8, 0x10000, 92, 0, 0,
                     1, 0xffff, 2 + 4096 // block, 0xffff - 4096 // block - 1)
    header[56:72] = bytes(range(16, 32))
    struct.pack_into('<QIII', header, 72, 2, 32, 128, zlib.crc32(entries[:4096]))
    struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
    sample = bytearray((i * 37 + 11) & 255 for i in range(4096))
    struct.pack_into('<I', sample, 1024, 0xf2f52010)
    path = out / f'gpt-{block}.bin'
    path.write_bytes(header + entries + sample)
    return path


def loader_checks(out):
    lib = out / 'adt.so'
    run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
         ROOT / 'enxerto/adt.c', '-o', lib])
    dll = ctypes.CDLL(str(lib))
    dll.adt_ufs_dma.argtypes = [ctypes.c_size_t] + [ctypes.c_uint64] * 7
    dll.adt_ufs_dma.restype = ctypes.c_int
    tree = build_tree(board='starlte')
    ufs = next(n for n in tree.children if n.name == 'sweet-ufs')
    assert ufs.prop('unipro-reg') == struct.pack('<QQ', 0x11110000, 0x8000)
    original = adt.serialize(tree)
    expected = fill_dma(tree, 0x81000000, 0x81300000, 0x81100000)
    args = [len(original), 0x81000000, 0x81300000, 0x81100000, 0x104000,
            0x880000000, 0x80000000]
    results = []

    def check(name, blob=original, values=None, result=-1, want=None):
        # Guards detect writes outside the ADT, including on rejected inputs.
        guard = b'\xa7' * 16
        buf = ctypes.create_string_buffer(guard + blob + guard)
        address = ctypes.addressof(buf) + 16
        values = args if values is None else values
        assert dll.adt_ufs_dma(address, *values) == result, name
        actual = ctypes.string_at(ctypes.addressof(buf), len(blob) + 32)
        assert actual == guard + (blob if want is None else want) + guard, name
        results.append(dict(case=name, status='pass'))

    check('live-image-pool', result=1, want=expected)
    check('already-patched', expected)
    for name, index, value in (
        ('short-header', 0, 4), ('short-tail', 0, len(original) - 4),
        ('image-empty', 2, 0x81000000), ('image-above-4G', 2, 0x100004000),
        ('pool-unaligned', 3, 0x81100001), ('pool-before-image', 3, 0x80ffc000),
        ('pool-overruns-image', 3, 0x812fc000), ('pool-too-small', 4, 0x100000),
        ('pool-too-large', 4, 0x108000), ('kernel-overlap', 5, 0x81000000),
        ('empty-ram', 6, 0), ('ram-wrap', 5, 0xffffffffffffc000),
    ):
        values = args.copy()
        values[index] = value
        check(name, values=values)
    for name, offset, value in (
        ('bad-property-count', 0, 0xffffffff), ('bad-child-count', 4, 0xffffffff),
        ('bad-property-size', 8 + 32, 0x7fffffff),
    ):
        blob = bytearray(original)
        struct.pack_into('<I', blob, offset, value)
        check(name, bytes(blob))
    for name, old, new in (
        ('wrong-board', b'samsung,starlte\0', b'samsung,starlto\0'),
        ('missing-pool', b'dma-pool\0', b'dmx-pool\0'),
        ('missing-image', b'loader-image\0', b'loader-imago\0'),
    ):
        assert original.count(old) == 1
        check(name, original.replace(old, new))
    tree = build_tree(board='starlte')
    tree.children.append(next(n for n in tree.children if n.name == 'sweet-ufs'))
    blob = adt.serialize(tree)
    check('duplicate-ufs', blob, [len(blob), *args[1:]])
    for board, qemu in (('sweet', False), ('beryllium', False), ('starlte', True)):
        blob = adt.serialize(build_tree(board=board, qemu=qemu))
        check(f'unchanged-{board}-qemu{qemu}', blob, [len(blob), *args[1:]], result=0)
    (out / 'adt-summary.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'PASS DMA handoff: {len(results)} ownership/ADT cases')
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu', type=Path, default=ROOT / 'research/starlte/xnu')
    ap.add_argument('--headers', type=Path, default=ROOT / 'research/starlte/header-check')
    ap.add_argument('--out', type=Path, default=ROOT / 'research/starlte/ufs-check')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    state = dict(status='running')
    try:
        state['boundary'] = host_source(args.xnu.resolve(), out)
        images = [fixture(out, block) for block in (512, 4096)]
        cmd = ['clang', '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-format',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-fno-omit-frame-pointer',
               '-I', out, HERE / 'ufs_host.c', '-lz', '-o', out / 'ufs-host']
        run(cmd)
        with (out / 'host.log').open('w') as log:
            result = subprocess.run([str(out / 'ufs-host'), *map(str, images)],
                                    stdout=log, stderr=subprocess.STDOUT)
        state['host'] = dict(command=list(map(str, cmd)), returncode=result.returncode)
        assert result.returncode == 0, f'see {out}/host.log'
        last_line = (out / 'host.log').read_text().splitlines()[-1]
        matched = re.fullmatch(r'PASS UFS host: (\d+) scenarios, byte-level protocol/DMA checks, ASan/UBSan', last_line)
        assert matched, last_line
        state['host']['scenarios'] = int(matched[1])
        print(last_line)
        state['adt'] = loader_checks(out)
        state['real_headers'] = headers.compile_source(args.xnu.resolve(), args.headers.resolve(),
                                                       'bsd/dev/sweet_ufs.c', out / 'sweet_ufs.o')
        assert state['real_headers']['returncode'] == 0
        state['status'] = 'pass'
    except BaseException as error:
        state.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        (out / 'summary.json').write_text(json.dumps(state, indent=2) + '\n')


if __name__ == '__main__':
    main()
