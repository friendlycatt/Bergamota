#!/usr/bin/env python3
"""Exercise Enxerto's real FDT parser, payload loader and boot_args producer on the host."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

from check import ROOT, build_tree, qemu, run
import adt
import mkpayload

HERE = Path(__file__).resolve().parent
PAGE = 0x4000
VIRT = 0xfffffff008000000


def compile_host(out, board, length):
    lib = out / f'{board}-{length}.so'
    flags = ['-DBOARD_' + board.upper()] if board != 'sweet' else []
    # Test each board's default ABI as well as the explicit override.
    default = 1024 if board == 'starlte' else 608
    if length != default:
        flags += [f'-DBOOT_ARGS_LINE_LENGTH={length}']
    run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared', *flags,
         '-I', ROOT / 'enxerto', HERE / 'enxerto_host.c',
         *[ROOT / 'enxerto' / f'{name}.c' for name in ('loader', 'fdt', 'adt', 'inflate')], '-o', lib])
    dll = ctypes.CDLL(str(lib))
    dll.test_boot.argtypes = [ctypes.c_size_t]
    dll.test_boot.restype = ctypes.c_char_p
    dll.fdt_board_info.argtypes = [ctypes.c_size_t]
    dll.loader_check.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
    dll.loader_check.restype = ctypes.c_char_p
    dll.fdt_root_compatible.argtypes = [ctypes.c_size_t, ctypes.c_char_p]
    dll.fdt_root_compatible.restype = ctypes.c_int
    return dll


def fill_dma(tree, image_lo, image_hi, pool):
    ufs = next(n for n in tree.children if n.name == 'sweet-ufs')
    values = {'dma-pool': struct.pack('<QQ', pool, 0x104000),
              'loader-image': struct.pack('<QQ', image_lo, image_hi - image_lo)}
    ufs.props = [(k, f, values.get(k, v)) for k, f, v in ufs.props]
    return adt.serialize(tree)


def synthetic_kernel(out, entry=b''):
    """Mach-O input for the real mkpayload producer: text, odd-sized data, BSS."""
    data = (b'Enxerto compressed segment\0' * 90000)[:(2 << 20) + 19]
    pieces = [(b'__TEXT', VIRT, PAGE, 0, 0x1000),
              (b'__DATA', VIRT + PAGE, len(data) + PAGE - len(data) % PAGE, 0x1000, len(data))]
    commands = b''
    for name, va, size, offset, file_size in pieces:
        commands += struct.pack('<II16s4Q4I', 0x19, 72, name, va, size, offset, file_size, 7, 7, 0, 0)
    registers = [0] * 34
    registers[32] = VIRT + 0x300
    commands += struct.pack('<4I34Q', 5, 288, 6, 68, *registers)
    header = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 3, len(commands), 0, 0)
    assert len(header + commands) <= 0x300 and len(entry) <= 0x1000 - 0x300
    image = (header + commands).ljust(0x300, b'\0') + entry
    image = image.ljust(0x1000, b'\0') + data
    path = out / 'synthetic.macho'
    path.write_bytes(image)
    return path, pieces, image


def make_fdt(out, start, end, root_size_cells, reserved_size_cells, initrd_cells=2):
    def cells(value, count):
        return ' '.join(f'0x{value >> shift & 0xffffffff:x}' for shift in range((count - 1) * 32, -1, -32))

    dts = f'''/dts-v1/;
/ {{
    #address-cells = <2>;
    #size-cells = <{root_size_cells}>;
    model = "Enxerto host fixture";
    memory@880000000 {{
        device_type = "memory";
        reg = <{cells(0x880000000, 2)} {cells(0x80000000, root_size_cells)}>;
    }};
    reserved-memory {{
        #address-cells = <2>;
        #size-cells = <{reserved_size_cells}>;
        ranges;
        display@cc000000 {{ reg = <{cells(0xcc000000, 2)} {cells(0x1043000, reserved_size_cells)}>; }};
    }};
    chosen {{
        linux,initrd-start = <{cells(start, initrd_cells)}>;
        linux,initrd-end = <{cells(end, initrd_cells)}>;
    }};
}};
'''
    # Cross-parent cell-count differences are intentional parser fixtures.
    result = run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', out / 'fixture.dtb'], input=dts, capture_output=True)
    assert not result.stdout
    return (out / 'fixture.dtb').read_bytes()


def compatible_checks(out, dll):
    def tree(root='', child=''):
        dts = f'/dts-v1/; / {{ {root} device {{ {child} }}; }};'
        path = out / 'compatible.dtb'
        run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', path], input=dts, capture_output=True)
        return path.read_bytes()

    identity = 'Samsung,EXYNOS9810'
    good = tree(f'compatible = "Samsung, Star rev26", "{identity}";')
    cases = [
        ('second-string', good, identity, 1),
        ('first-string', good, 'Samsung, Star rev26', 1),
        ('prefix', good, 'Samsung,EXYNOS981', 0),
        ('suffix', good, 'EXYNOS9810', 0),
        ('wrong-case', good, 'samsung,exynos9810', 0),
        ('qemu', tree('compatible = "linux,dummy-virt";'), identity, 0),
        ('child-only', tree(child=f'compatible = "{identity}";'), identity, 0),
        ('missing', tree(), identity, 0),
        ('empty-list', tree('compatible;'), identity, 0),
        ('lowercase', tree('compatible = "samsung,exynos9810";'), 'samsung,exynos9810', 1),
        ('longer-entry', tree(f'compatible = "{identity}-extra";'), identity, 0),
    ]
    struct_offset, = struct.unpack_from('>I', good, 8)
    prop = struct_offset + 8  # Empty root name, then its first property.
    assert struct.unpack_from('>I', good, prop)[0] == 3
    for name, offset, value in (
        ('bad-magic', 0, 0), ('short-header', 4, 36), ('v16', 20, 16),
        ('unaligned-struct', 8, struct_offset + 1), ('struct-outside', 36, len(good)),
        ('strings-outside', 32, len(good)), ('bad-nameoff', prop + 8, len(good)),
        ('oversized-value', prop + 4, 0xffffffff),
    ):
        bad = bytearray(good)
        struct.pack_into('>I', bad, offset, value)
        cases.append((name, bytes(bad), identity, 0))
    bad = good.replace(identity.encode() + b'\0', identity.encode() + b'X')
    cases += [('unterminated-entry', bad, identity, 0),
              ('matched-before-bad-tail', bad, 'Samsung, Star rev26', 0)]
    bad = good.replace(b'compatible\0', b'compatibleX')
    cases.append(('unterminated-name', bad, identity, 0))
    results = []
    for name, data, query, expected in cases:
        buf = ctypes.create_string_buffer(data)
        assert dll.fdt_root_compatible(ctypes.addressof(buf), query.encode()) == expected, name
        results.append(dict(case=name, expected=expected, status='pass'))
    buf = ctypes.create_string_buffer(good)
    for name, address in (('null', 0), ('unaligned', ctypes.addressof(buf) + 1)):
        assert dll.fdt_root_compatible(address, identity.encode()) == 0, name
        results.append(dict(case=name, expected=0, status='pass'))
    (out / 'compatible-summary.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'PASS FDT MMIO gate: {len(results)} exact root-list / malformed input cases')


def host_checks(out):
    kernel, pieces, original = synthetic_kernel(out)
    results = []
    for board in ('sweet', 'beryllium', 'starlte'):
        for length in (608, 1024):
            dll = compile_host(out, board, length)
            for version in (1, 2):
                size = 4 << 20
                memory = ctypes.create_string_buffer(size + PAGE)
                phys = (ctypes.addressof(memory) + PAGE - 1) & ~(PAGE - 1)
                ctypes.memset(phys, 0xa5, size)
                tree = build_tree(board=board)
                chosen = next(n for n in tree.children if n.name == 'chosen')
                chosen.props = [(k, f, struct.pack('<Q', phys if k == 'dram-base' else size)
                                 if k in ('dram-base', 'dram-size') else v) for k, f, v in chosen.props]
                adt_path = out / 'fixture.adt'
                adt_path.write_bytes(adt.serialize(tree))
                cmdline = 'debug=0xc ' + 'x' * 597  # 607 bytes plus NUL: fill the payload field.
                blob, _ = mkpayload.build(kernel, adt_path, cmdline, compress=version == 2)
                # Payload alignment padding is not segment data or BSS.
                blob = bytearray(blob)
                stride = 40 if version == 2 else 32
                for i in range(len(pieces)):
                    offset, count = struct.unpack_from('<QQ', blob, 0x2a8 + i * stride + 16)
                    tail = offset + count
                    blob[tail:(tail + 7) & ~7] = b'\xcc' * (-tail % 8)
                blob = bytes(blob)
                payload = ctypes.create_string_buffer(blob)
                start = ctypes.addressof(payload)
                end = start + len(blob)
                assert dll.loader_check(start, end) is None
                fdt = ctypes.create_string_buffer(make_fdt(out, start, end, 1 if version == 1 else 2, 2 if version == 1 else 1))
                why = dll.test_boot(ctypes.addressof(fdt))
                assert why is None, why
                assert ctypes.c_uint64.in_dll(dll, 'captured_entry').value == phys + 0x300
                for name, va, vm_size, offset, file_size in pieces:
                    actual = ctypes.string_at(phys + va - VIRT, vm_size)
                    expected = original[offset:offset + file_size].ljust(vm_size, b'\0')
                    if actual != expected:
                        first = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
                        raise AssertionError(f'{board}/{length}/v{version}: {name.decode()} load/BSS mismatch at {first:#x}')
                ba = ctypes.c_uint64.in_dll(dll, 'captured_args').value
                args = ctypes.string_at(ba, length + 128)
                assert struct.unpack_from('<HH', args) == (2, 2)
                assert struct.unpack_from('<4Q', args, 8) == (VIRT, phys, size, ba + PAGE)
                assert args[108:108 + length] == (cmdline.encode() + b'\0').ljust(length, b'\0')
                assert struct.unpack_from('<QQ', args, length + 112) == (0, size)
                adt_off, adt_size = struct.unpack_from('<QQ', blob, 0x20)
                adt_va = struct.unpack_from('<Q', args, 96)[0]
                payload_tree, _ = adt.parse(blob[adt_off:adt_off + adt_size])
                expected_adt = (fill_dma(payload_tree, 0x81000000, 0x81300000, 0x81100000)
                                if board == 'starlte' else blob[adt_off:adt_off + adt_size])
                assert ctypes.string_at(phys + adt_va - VIRT, adt_size) == expected_adt
                assert ctypes.string_at(start, len(blob)) == blob, 'modified source payload'
                assert ctypes.c_uint.in_dll(dll, 'charger_handoffs').value == (board != 'starlte')
                diagnostics = (ctypes.c_char * 8192).in_dll(dll, 'diagnostics')
                diagnostics.value = b''
                dll.fdt_board_info(ctypes.addressof(fdt))
                assert b'MEMORY: 0000000880000000 +0000000080000000' in diagnostics.value
                assert b'display@cc0: 00000000CC000000 +0000000001043000' in diagnostics.value
                # The FDT-initrd path must validate before copying any malformed segment.
                struct.pack_into('<I', payload, 12, 65)
                ctypes.memset(phys, 0xa5, size)
                assert dll.test_boot(ctypes.addressof(fdt)) == b'bad segment table'
                assert ctypes.string_at(phys, size) == b'\xa5' * size
                # Destination/image overlap is rejected before segment copies.
                struct.pack_into('<I', payload, 12, len(pieces))
                ctypes.c_uint64.in_dll(dll, 'test_image_lo').value = phys
                ctypes.c_uint64.in_dll(dll, 'test_image_hi').value = phys + size
                assert dll.test_boot(ctypes.addressof(fdt)) == b'payload overlaps Enxerto image'
                assert ctypes.string_at(phys, size) == b'\xa5' * size
                ctypes.c_uint64.in_dll(dll, 'test_image_lo').value = 0x81000000
                ctypes.c_uint64.in_dll(dll, 'test_image_hi').value = 0x81300000
                results.append(dict(board=board, boot_args=length, payload=version, status='pass'))
                print(f'PASS Enxerto host: {board}, boot_args={length}, payload v{version}, FDT/BSS/handoff')
            # Accept 32-bit /chosen properties as well as the 64-bit fixture form.
            fdt = ctypes.create_string_buffer(make_fdt(out, 0x48000000, 0x48010000, 2, 2, initrd_cells=1))
            value = ctypes.c_uint64()
            dll.fdt_chosen_u64.argtypes = [ctypes.c_size_t, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64)]
            assert dll.fdt_chosen_u64(ctypes.addressof(fdt), b'linux,initrd-start', ctypes.byref(value)) == 1
            assert value.value == 0x48000000
    (out / 'host-summary.json').write_text(json.dumps(results, indent=2) + '\n')
    compatible_checks(out, dll)


def build_loader(out, display_probe):
    out.mkdir(exist_ok=True)
    env = {**os.environ, 'BOARD': 'starlte', 'BOOT_ARGS_LINE_LENGTH': '1024',
           'BOOTLOG': '1', 'STARLTE_DISPLAY_PROBE': str(int(display_probe))}
    env.pop('BOOTPAUSE', None)
    run(['bash', ROOT / 'enxerto/build.sh'], env=env)
    for name in ('Image', 'enxerto.elf'):
        shutil.copy2(ROOT / 'enxerto/out/starlte' / name, out / name)
    image = out / 'Image'
    header = image.read_bytes()
    assert header[56:60] == b'ARM\x64' and struct.unpack_from('<Q', header, 16)[0] == len(header)
    nm = run(['llvm-nm', '--defined-only', out / 'enxerto.elf'], capture_output=True).stdout
    symbols = {fields[2]: int(fields[0], 16) for line in nm.splitlines()
               if len(fields := line.split()) == 3}
    (out / 'build.json').write_text(json.dumps(dict(
        display_probe=display_probe, image_bytes=len(header),
        image_sha256=hashlib.sha256(header).hexdigest(),
        flags={key: env[key] for key in ('BOARD', 'BOOT_ARGS_LINE_LENGTH', 'BOOTLOG', 'STARLTE_DISPLAY_PROBE')},
    ), indent=2) + '\n')
    return image, header, symbols


def capture_console(monitor, case, header, symbols):
    # QEMU virt places this zero-text-offset, anywhere-placement Image at
    # RAM + 2 MiB. Assert its header before interpreting private console RAM.
    path = case / 'loader-ram.bin'
    qemu.qmp(str(monitor), pmemsave=(0x40200000, len(header), str(path)))
    ram = path.read_bytes()
    assert ram[:64] == header[:64], 'unexpected QEMU Image placement'
    count, = struct.unpack_from('<I', ram, symbols['hist_n'])
    lines = []
    for i in range(max(0, count - 47), count):
        offset = symbols['hist_ch'] + i % 48 * 64
        lines.append(ram[offset:offset + 64].rstrip(b'\0').replace(b'\0', b' ').decode('ascii'))
    text = '\n'.join(lines) + '\n'
    (case / 'console.txt').write_text(text)
    return text


def check_entry_console(text, entry_el):
    assert f'in={entry_el:08X} now=00000001' in text, text
    assert 'skipped: non-Exynos FDT' in text, text
    assert 'SCTLRin' in text and 'IMAGE   0000000040200000' in text, text


def display_probe_checks(out, kernel):
    image, header, symbols = build_loader(out / 'display-probe-loader', True)
    tree = build_tree(board='starlte', qemu=True, gic_version=2, qemu_cpus=1)
    adt_path = out / 'display-probe.adt'
    adt_path.write_bytes(adt.serialize(tree))
    blob, _ = mkpayload.build(kernel, adt_path, 'debug=0xc', compress=True)
    payload = out / 'display-probe-payload.bin'
    payload.write_bytes(blob)
    sentinel = out / 'destination-sentinel.bin'
    sentinel.write_bytes(b'\xa5' * PAGE)
    results = []
    for el2 in (False, True):
        case = out / f'display-probe-el{2 if el2 else 1}'
        case.mkdir(exist_ok=True)
        monitor = case / 'qmp.sock'
        monitor.unlink(missing_ok=True)
        serial = case / 'serial.txt'
        serial.write_text('')
        cmd = ['qemu-system-aarch64', '-M', f'virt,gic-version=2,virtualization={"on" if el2 else "off"}',
               '-cpu', 'cortex-a76', '-m', '4G', '-nographic', '-serial', f'file:{serial}',
               '-qmp', f'unix:{monitor},server=on,wait=off', '-no-reboot',
               '-d', 'int,guest_errors', '-D', str(case / 'qemu.log'),
               '-kernel', str(image), '-initrd', str(payload),
               '-device', f'loader,file={sentinel},addr=0x50000000,force-raw=on']
        (case / 'command.json').write_text(json.dumps(cmd, indent=2) + '\n')
        with (case / 'runner.log').open('w') as log:
            proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 20
                text = ''
                while proc.poll() is None and time.monotonic() < deadline:
                    if monitor.exists():
                        text = capture_console(monitor, case, header, symbols)
                        if 'READY - photograph this screen' in text:
                            break
                    time.sleep(0.1)
                assert proc.poll() is None, f'{case}: QEMU exited ({proc.returncode})'
                regs = qemu.qmp(str(monitor), 'stop', 'info registers', cpu_index=0)
                (case / 'regs.txt').write_text(regs[-1])
                text = capture_console(monitor, case, header, symbols)
                assert 'DISPLAY PROBE 1' in text and 'READY - photograph this screen' in text, text
                check_entry_console(text, 2 if el2 else 1)
                assert 'EL1h' in regs[-1], regs[-1]
                qemu.qmp(str(monitor), pmemsave=(0x50000000, PAGE, str(case / 'destination.bin')))
                assert (case / 'destination.bin').read_bytes() == sentinel.read_bytes(), 'probe loaded XNU destination'
                assert not serial.read_text(), 'probe executed the payload'
                fb = qemu.framebuffer(blob)
                qemu.qmp(str(monitor), pmemsave=(fb.base, fb.size, str(case / 'fb.raw')))
                pixels = (case / 'fb.raw').read_bytes()
                for x, expected in ((240, 0xffff0000), (720, 0xff00ff00), (1200, 0xff0000ff)):
                    assert struct.unpack_from('<I', pixels, 32 * fb.stride + x * 4)[0] == expected
                qemu.write_png(case / 'fb.png', pixels, fb)
                (case / 'fb.raw').unlink()
                errors = (case / 'qemu.log').read_text()
                assert 'Invalid ' not in errors and 'Abort' not in errors, errors
                results.append(dict(entry_el=2 if el2 else 1, status='pass', image_size=len(header),
                                    framebuffer_bars='RGB', destination='untouched', samsung_mmio='skipped'))
                print(f'PASS display probe: EL{2 if el2 else 1}, visible RGB/entry diagnostics, destination untouched')
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
                monitor.unlink(missing_ok=True)
    (out / 'display-probe-summary.json').write_text(json.dumps(results, indent=2) + '\n')


def qemu_checks(out):
    image, header, symbols = build_loader(out / 'boot-loader', False)
    run(['clang', '--target=aarch64-none-elf', '-c', HERE / 'enxerto_probe.S', '-o', out / 'probe.o'])
    run(['ld.lld', '--image-base=0', '-Ttext=0', '--entry=_start', out / 'probe.o', '-o', out / 'probe.elf'])
    run(['llvm-objcopy', '-O', 'binary', out / 'probe.elf', out / 'probe.bin'])
    kernel, _, _ = synthetic_kernel(out, (out / 'probe.bin').read_bytes())
    results = []
    for gic, el2, version in ((2, False, 1), (2, True, 2), (3, False, 2), (3, True, 1)):
        case = out / f'gic{gic}-el{2 if el2 else 1}-v{version}'
        case.mkdir(exist_ok=True)
        tree = build_tree(board='starlte', qemu=True, gic_version=gic)
        dma_case = gic == 2 and el2
        if dma_case:
            # The synthetic payload only records a handoff; it never accesses
            # these Samsung registers. Exercise the real loader's DMA export.
            tree.children.append(next(n for n in build_tree(board='starlte').children if n.name == 'sweet-ufs'))
        adt_path = case / 'fixture.adt'
        adt_path.write_bytes(adt.serialize(tree))
        blob, _ = mkpayload.build(kernel, adt_path, 'debug=0xc', compress=version == 2)
        payload = case / 'payload.bin'
        payload.write_bytes(blob)
        phys, expected, args, _ = qemu.build_image(blob)
        if dma_case:
            image_lo = 0x40200000
            image_hi = (image_lo + len(header) + PAGE - 1) & ~(PAGE - 1)
            pool = (image_lo + symbols['ufs_dma_arena'] + PAGE - 1) & ~(PAGE - 1)
            assert image_lo <= pool and pool + 0x104000 <= image_lo + len(header)
            adt_off, adt_size = struct.unpack_from('<QQ', blob, 0x20)
            payload_tree, _ = adt.parse(blob[adt_off:adt_off + adt_size])
            filled = fill_dma(payload_tree, image_lo, image_hi, pool)
            adt_at = struct.unpack_from('<Q', blob, 0x30)[0] - VIRT
            expected = bytearray(expected)
            expected[adt_at:adt_at + len(filled)] = filled
            expected = bytes(expected)
        monitor = case / 'qmp.sock'
        monitor.unlink(missing_ok=True)
        serial = case / 'serial.txt'
        serial.write_text('')
        cmd = ['qemu-system-aarch64', '-M', f'virt,gic-version={gic},virtualization={"on" if el2 else "off"}',
               '-cpu', 'cortex-a76', '-m', '4G', '-nographic', '-serial', f'file:{serial}',
               '-qmp', f'unix:{monitor},server=on,wait=off', '-no-reboot',
               '-d', 'int,guest_errors', '-D', str(case / 'qemu.log'),
               '-kernel', str(image), '-initrd', str(payload)]
        (case / 'command.json').write_text(json.dumps(cmd, indent=2) + '\n')
        with (case / 'runner.log').open('w') as log:
            proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 20
                while proc.poll() is None and time.monotonic() < deadline:
                    if 'ENXERTO HANDOFF' in serial.read_text():
                        break
                    time.sleep(0.05)
                assert proc.poll() is None, f'{case}: QEMU exited ({proc.returncode})'
                regs = qemu.qmp(str(monitor), 'stop', 'info registers',
                                pmemsave=(0x4ff00000, 64, str(case / 'report.bin')))
                (case / 'regs.txt').write_text(regs[-1])
                qemu.qmp(str(monitor), pmemsave=(phys, len(expected), str(case / 'ram.bin')))
                fb = qemu.framebuffer(blob)
                qemu.qmp(str(monitor), pmemsave=(fb.base, fb.size, str(case / 'fb.raw')))
                qemu.write_png(case / 'fb.png', (case / 'fb.raw').read_bytes(), fb)
                (case / 'fb.raw').unlink()
                assert 'ENXERTO HANDOFF' in serial.read_text(), f'{case}: no payload handoff; see regs.txt/fb.png'
                report = struct.unpack('<8Q', (case / 'report.bin').read_bytes())
                assert report[:3] == (0x4558, args, 4), report
                assert report[3] & 0x1005 == 0, f'SCTLR MMU/caches left on: {report[3]:#x}'
                # Page tables were installed during loading; the final jump left them disabled.
                assert report[5] == 0x1409cb51c and 0x40000000 <= report[6] < 0x48000000, report
                assert report[7] == 0x3c0, f'interrupts unmasked: {report[7]:#x}'
                actual = (case / 'ram.bin').read_bytes()
                assert actual == expected, f'{case}: loaded RAM differs from host reference'
                check_entry_console(capture_console(monitor, case, header, symbols), 2 if el2 else 1)
                errors = (case / 'qemu.log').read_text()
                assert 'Invalid ' not in errors and 'Abort' not in errors, errors
                result = dict(gic=gic, entry_el=2 if el2 else 1, payload=version, status='pass',
                              boot_args=args, image_size=len(header), handoff_el=report[2] >> 2,
                              sctlr=report[3], tcr=report[5], ttbr0=report[6],
                              dma_handoff=(dict(base=pool, size=0x104000) if dma_case else None))
                results.append(result)
                print(f'PASS Enxerto QEMU: GICv{gic}, EL{2 if el2 else 1} entry, payload v{version}, exact RAM/boot_args, MMU off')
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
                monitor.unlink(missing_ok=True)
    (out / 'qemu-summary.json').write_text(json.dumps(results, indent=2) + '\n')
    display_probe_checks(out, kernel)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'research/starlte/enxerto-check')
    ap.add_argument('--qemu', action='store_true', help='also build and execute the actual Samsung Enxerto Image')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    host_checks(out)
    if args.qemu:
        qemu_checks(out)


if __name__ == '__main__':
    main()
