#!/usr/bin/env python3
"""Exercise Enxerto's real FDT parser, payload loader and boot_args producer on the host."""
import argparse
import ctypes
import json
import os
from pathlib import Path
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
         *[ROOT / 'enxerto' / f'{name}.c' for name in ('loader', 'fdt', 'inflate')], '-o', lib])
    dll = ctypes.CDLL(str(lib))
    dll.test_boot.argtypes = [ctypes.c_size_t]
    dll.test_boot.restype = ctypes.c_char_p
    dll.fdt_board_info.argtypes = [ctypes.c_size_t]
    dll.loader_check.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
    dll.loader_check.restype = ctypes.c_char_p
    return dll


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
                assert ctypes.string_at(phys + adt_va - VIRT, adt_size) == blob[adt_off:adt_off + adt_size]
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
                results.append(dict(board=board, boot_args=length, payload=version, status='pass'))
                print(f'PASS Enxerto host: {board}, boot_args={length}, payload v{version}, FDT/BSS/handoff')
            # Accept 32-bit /chosen properties as well as the 64-bit fixture form.
            fdt = ctypes.create_string_buffer(make_fdt(out, 0x48000000, 0x48010000, 2, 2, initrd_cells=1))
            value = ctypes.c_uint64()
            dll.fdt_chosen_u64.argtypes = [ctypes.c_size_t, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64)]
            assert dll.fdt_chosen_u64(ctypes.addressof(fdt), b'linux,initrd-start', ctypes.byref(value)) == 1
            assert value.value == 0x48000000
    (out / 'host-summary.json').write_text(json.dumps(results, indent=2) + '\n')


def qemu_checks(out):
    run(['bash', ROOT / 'enxerto/build.sh'], env={**os.environ, 'BOARD': 'starlte', 'BOOT_ARGS_LINE_LENGTH': '1024'})
    image = ROOT / 'enxerto/out/starlte/Image'
    header = image.read_bytes()
    assert header[56:60] == b'ARM\x64' and struct.unpack_from('<Q', header, 16)[0] == len(header)
    run(['clang', '--target=aarch64-none-elf', '-c', HERE / 'enxerto_probe.S', '-o', out / 'probe.o'])
    run(['ld.lld', '--image-base=0', '-Ttext=0', '--entry=_start', out / 'probe.o', '-o', out / 'probe.elf'])
    run(['llvm-objcopy', '-O', 'binary', out / 'probe.elf', out / 'probe.bin'])
    kernel, _, _ = synthetic_kernel(out, (out / 'probe.bin').read_bytes())
    results = []
    for gic, el2, version in ((2, False, 1), (2, True, 2), (3, False, 2), (3, True, 1)):
        case = out / f'gic{gic}-el{2 if el2 else 1}-v{version}'
        case.mkdir(exist_ok=True)
        tree = build_tree(board='starlte', qemu=True, gic_version=gic)
        adt_path = case / 'fixture.adt'
        adt_path.write_bytes(adt.serialize(tree))
        blob, _ = mkpayload.build(kernel, adt_path, 'debug=0xc', compress=version == 2)
        payload = case / 'payload.bin'
        payload.write_bytes(blob)
        phys, expected, args, _ = qemu.build_image(blob)
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
                result = dict(gic=gic, entry_el=2 if el2 else 1, payload=version, status='pass',
                              boot_args=args, image_size=len(header), handoff_el=report[2] >> 2,
                              sctlr=report[3], tcr=report[5], ttbr0=report[6])
                results.append(result)
                print(f'PASS Enxerto QEMU: GICv{gic}, EL{2 if el2 else 1} entry, payload v{version}, exact RAM/boot_args, MMU off')
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
                monitor.unlink(missing_ok=True)
    (out / 'qemu-summary.json').write_text(json.dumps(results, indent=2) + '\n')


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
