#!/usr/bin/env python3
"""Boot a SWEETXNU payload (tools/mkpayload.py) in QEMU's virt board.

  qemu-sweet.py PAYLOAD OUTDIR [--secs N] [--gic-version 2|3] [--cpus N]

Does what enxerto/loader.c does on the phone, but on the host: places the
kernel segments, ADT and boot_args at their physical addresses and starts
the CPU at a 6-instruction stub that jumps to the entry with x0 = boot_args.
GIC version, CPU count and framebuffer geometry come from a --qemu ADT.
Both SWEETXNU payload versions (plain / raw DEFLATE) are supported.
boot_args uses XNU 26's 1024-byte CommandLine; older kernels can select 608.

After N seconds (or when the guest stops) it writes into OUTDIR:
  fb.png     the ADT-described framebuffer
  regs.txt   QEMU's view of CPU state
  qemu.log   exceptions with ESR/FAR/ELR (-d int,guest_errors)

With --kernel KERNEL (the matching unstripped Mach-O), also decode the
framebuffer using its iso_font into fb.txt and write result.json. This is a
snapshot of the circular screen, not a complete boot log. --fail-on-panic
returns 2 when a panic is visible; absence of a panic does not prove boot.
An undecodable or blank screen returns 3 with --fail-on-panic.
"""
import argparse, json, os, shutil, socket, struct, subprocess, sys, time, zlib
from typing import NamedTuple

import adt

PAGE = 0x4000
STUB_PA = 0x40200000	# above the dtb QEMU places at RAM base
PAYLOAD_LINE_LENGTH = 608


class Framebuffer(NamedTuple):
    base: int
    width: int
    height: int
    stride: int

    @property
    def size(self):
        return self.stride * self.height


def payload_header(payload):
    if len(payload) < 0x2a8:
        raise ValueError('payload header truncated')
    magic, version, count = struct.unpack_from('<8sII', payload)
    if magic != b'SWEETXNU' or version not in (1, 2):
        raise ValueError('not a SWEETXNU v1/v2 payload')
    stride = 40 if version == 2 else 32
    if not 1 <= count <= 64 or 0x2a8 + count * stride > len(payload):
        raise ValueError('bad payload segment table')
    values = struct.unpack_from('<7Q', payload, 0x10)
    _, _, offset, size, _, _, _ = values
    if offset < 0x2a8 + count * stride or offset + size > len(payload):
        raise ValueError('ADT outside payload')
    return version, count, values


def payload_segments(payload):
    version, count, _ = payload_header(payload)
    stride = 40 if version == 2 else 32
    for i in range(count):
        fields = struct.unpack_from('<5Q' if version == 2 else '<4Q', payload, 0x2a8 + stride * i)
        va, size, offset, file_size = fields[:4]
        raw_size = fields[4] if version == 2 else 0
        if offset + file_size > len(payload) or (raw_size or file_size) > size:
            raise ValueError(f'segment {i} outside payload or larger than vmsize')
        data = payload[offset:offset + file_size]
        if raw_size:
            decoder = zlib.decompressobj(-15)
            data = decoder.decompress(data, raw_size + 1)
            if len(data) != raw_size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                raise ValueError(f'segment {i} has invalid raw DEFLATE data/size')
        yield va, size, data


def payload_tree(payload):
    _, _, (_, _, offset, size, _, _, _) = payload_header(payload)
    tree, end = adt.parse(payload[offset:offset + size])
    if end != size:
        raise ValueError('ADT size mismatch')
    return tree


def tree_node(tree, path):
    node = tree
    for part in path.strip('/').split('/'):
        node = next((c for c in node.children if c.name == part), None)
        if node is None:
            raise ValueError(f'ADT missing {path}')
    return node


def framebuffer(payload):
    node = tree_node(payload_tree(payload), '/sweet-fbcon')
    base, reserved = struct.unpack('<2Q', node.prop('reg'))
    width, height, stride = (struct.unpack('<I', node.prop(key))[0] for key in ('width', 'height', 'stride'))
    fb = Framebuffer(base, width, height, stride)
    if not base or width <= 0 or height <= 0 or stride % 4 or stride < width * 4 or fb.size > reserved:
        raise ValueError('invalid ADT framebuffer geometry')
    return fb


def qemu_config(payload):
    """Reject hardware ADTs rather than silently running with the wrong MMIO/CPUs."""
    tree = payload_tree(payload)
    io = tree_node(tree, '/arm-io')
    gic = tree_node(tree, '/arm-io/gic')
    compat = gic.prop('compatible').split(b'\0')
    if b'arm,cortex-a15-gic' in compat or b'arm,cortex-a9-gic' in compat:
        version, cpu_base = 2, 0x08010000
    elif b'arm,gic-v3' in compat:
        version, cpu_base = 3, 0x080a0000
    else:
        raise ValueError('unsupported ADT interrupt controller')
    bus, soc, _ = struct.unpack('<3Q', io.prop('ranges'))
    dist, _, interface, _ = struct.unpack('<4Q', gic.prop('reg'))
    if bus or dist + soc != 0x08000000 or interface + soc != cpu_base:
        raise ValueError('GIC addresses are not QEMU virt; regenerate ADT with --qemu')
    if gic.prop('virtual-timer-intid') != struct.pack('<I', 27):
        raise ValueError('QEMU requires virtual timer INTID 27')
    if tree_node(tree, '/psci').prop('method') != b'hvc\0':
        raise ValueError('QEMU virt requires PSCI HVC')
    cpus = tree_node(tree, '/cpus').children
    ids = [struct.unpack('<I', c.prop('reg'))[0] for c in cpus]
    if not 1 <= len(ids) <= 8 or ids != list(range(len(ids))):
        raise ValueError('QEMU requires contiguous MPIDRs; regenerate ADT with --qemu --qemu-cpus N')
    return version, len(ids)


def check_qemu_dependency(cpu='cortex-a76'):
    """Verify qemu-system-aarch64 binary and CPU model support."""
    qemu_bin = shutil.which('qemu-system-aarch64')
    if not qemu_bin:
        print('[!] Error: qemu-system-aarch64 not found in PATH.', file=sys.stderr)
        print('    Install instructions:', file=sys.stderr)
        print('      Arch Linux:    sudo pacman -S qemu-system-aarch64', file=sys.stderr)
        print('      Ubuntu/Debian: sudo apt install qemu-system-arm', file=sys.stderr)
        print('      Fedora:         sudo dnf install qemu-system-aarch64', file=sys.stderr)
        print('      macOS:          brew install qemu', file=sys.stderr)
        return False
    try:
        ver = subprocess.check_output([qemu_bin, '--version'], text=True).splitlines()[0]
        print(f'[*] Found QEMU: {ver}', file=sys.stderr)
    except Exception as e:
        print(f'[!] Warning: Failed to query QEMU version: {e}', file=sys.stderr)
        return False

    try:
        cpu_list = subprocess.check_output([qemu_bin, '-M', 'virt', '-cpu', 'help'], text=True)
        if cpu in cpu_list:
            print(f'[*] CPU model {cpu} supported', file=sys.stderr)
        else:
            print(f'[!] Warning: CPU model {cpu} not explicitly listed in qemu-system-aarch64 -cpu help', file=sys.stderr)
    except Exception:
        pass
    return True


def check_kernel_tools(kernel_path):
    """Verify llvm-nm is available when --kernel is specified."""
    if not shutil.which('llvm-nm'):
        print('[!] Error: llvm-nm not found in PATH, required for --kernel font extraction.', file=sys.stderr)
        print('    Install instructions:', file=sys.stderr)
        print('      Arch Linux:    sudo pacman -S llvm', file=sys.stderr)
        print('      Ubuntu/Debian: sudo apt install llvm', file=sys.stderr)
        print('      Fedora:         sudo dnf install llvm', file=sys.stderr)
        print('      macOS:          brew install llvm', file=sys.stderr)
        return False
    if not os.path.isfile(kernel_path):
        print(f'[!] Error: kernel file does not exist: {kernel_path}', file=sys.stderr)
        return False
    return True


def build_image(payload, boot_args_line_length=1024):
    """Return (phys_base, ram_image, boot_args_pa, entry_pa)."""
    _, _, (virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size) = payload_header(payload)
    if boot_args_line_length not in (608, 1024):
        raise ValueError('boot_args command-line length must be 608 or 1024')
    cmdline = payload[0x48:0x48 + PAYLOAD_LINE_LENGTH]
    if b'\0' not in cmdline:
        raise ValueError('payload command line is not NUL terminated')
    fb = framebuffer(payload)
    delta = phys_base - virt_base
    adt_pa = kernel_end + delta
    ba_pa = (adt_pa + adt_size + PAGE - 1) & ~(PAGE - 1)
    top = ba_pa + PAGE
    if not mem_size or phys_base >= adt_pa or top > phys_base + mem_size:
        raise ValueError('kernel/ADT/boot_args outside RAM')
    if phys_base < fb.base + fb.size and fb.base < phys_base + mem_size:
        raise ValueError('framebuffer overlaps kernel-managed RAM')
    img = bytearray(top - phys_base)
    spans, entry_found = [], False
    for vmaddr, vmsize, data in payload_segments(payload):
        o = vmaddr - virt_base
        if o < 0 or vmaddr + vmsize > kernel_end:
            raise ValueError('segment outside kernel RAM')
        if any(o < end and start < o + vmsize for start, end in spans):
            raise ValueError('overlapping payload segments')
        spans.append((o, o + vmsize))
        entry_found |= vmaddr <= entry_va < vmaddr + len(data)
        img[o:o + len(data)] = data
    if not entry_found:
        raise ValueError('entry point outside initialized segments')
    img[adt_pa - phys_base:adt_pa - phys_base + adt_size] = payload[adt_off:adt_off + adt_size]
    ba = struct.pack('<HH4x4Q6QI4xQI', 2, 2, virt_base, phys_base, mem_size, top,
                     fb.base, 1, fb.stride, fb.width, fb.height, 32, 0, adt_pa - delta, adt_size)
    ba += cmdline.ljust(boot_args_line_length, b'\0') + b'\0' * 4 + struct.pack('<QQ', 0, mem_size)
    assert len(ba) == 128 + boot_args_line_length
    img[ba_pa - phys_base:ba_pa - phys_base + len(ba)] = ba
    return phys_base, bytes(img), ba_pa, entry_va + delta


def stub(ba_pa, entry_pa):
    # ldr x0, =ba ; ldr x1, =entry ; br x1 ; nop ; .quad ba ; .quad entry
    return struct.pack('<4I2Q', 0x58000080, 0x580000a1, 0xd61f0020, 0xd503201f, ba_pa, entry_pa)


def write_png(path, raw, fb):
    rows = []
    for y in range(fb.height):
        line = raw[y * fb.stride:y * fb.stride + fb.width * 4]
        rgb = bytearray(fb.width * 3)	# a8r8g8b8 little endian = B G R A in memory
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
        rows.append(b'\0' + bytes(rgb))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', fb.width, fb.height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def payload_data(payload, va, size):
    """Read initialized segment bytes by VA (the payload has no slide)."""
    for vmaddr, _, data in payload_segments(payload):
        if vmaddr <= va and va + size <= vmaddr + len(data):
            offset = va - vmaddr
            return data[offset:offset + size]
    raise ValueError(f'VA {va:#x}+{size:#x} not in initialized payload segments')


def decode_screen(raw, font, fb):
    """Exact glyph matching for pe_serial.c's 2x, white-on-black fbcon."""
    glyphs = {font[c * 16:(c + 1) * 16]: chr(c) for c in range(32, 127)}
    rows, unknown = [], 0
    for row in range(fb.height // 32):
        chars = []
        for col in range(fb.width // 16):
            glyph = bytearray(16)
            for y in range(16):
                offset = (row * 32 + y * 2) * fb.stride + col * 16 * 4
                for x in range(8):
                    if raw[offset + x * 8:offset + x * 8 + 4] == b'\xff' * 4:
                        glyph[y] |= 1 << x
            key = bytes(glyph)
            char = glyphs.get(key, '?')
            unknown += key not in glyphs
            chars.append(char)
        rows.append(''.join(chars).rstrip())
    return '\n'.join(rows).rstrip() + '\n', unknown


def screen_result(screen, unknown):
    if 'panic(cpu ' in screen or 'SWEET EARLY PANIC:' in screen:
        status = 'panic'
    elif unknown:
        status = 'incomplete_decode'
    elif screen.strip():
        status = 'no_panic_observed'
    else:
        status = 'blank_screen'
    return {'status': status, 'unknown_glyphs': unknown, 'snapshot_only': True}


def qmp(sock_path, *cmds, pmemsave=None, cpu_index=None):
    """Run QEMU human-monitor commands over QMP (synchronous, unlike the echoing HMP socket)."""
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    f = s.makefile('rw')
    f.readline()

    def call(execute, **args):
        f.write(json.dumps({'execute': execute, 'arguments': args}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                return r

    call('qmp_capabilities')
    out = []
    for c in cmds:
        args = {'command-line': c}
        # QMP resets HMP's selected CPU for each command. `cpu N` followed by
        # `info registers` therefore still reads CPU0; pass the QMP selector.
        if cpu_index is not None:
            args['cpu-index'] = cpu_index
        r = call('human-monitor-command', **args)
        if 'error' in r:
            print(f'qmp {c!r}: {r["error"]}', file=sys.stderr)
        out.append(r.get('return', ''))
    if pmemsave:	# native QMP: HMP's argument parser chokes on '-' in paths
        r = call('pmemsave', val=pmemsave[0], size=pmemsave[1], filename=pmemsave[2])
        if 'error' in r:
            print(f'qmp pmemsave: {r["error"]}', file=sys.stderr)
    s.close()
    return out


def stream_serial_file(serial_path, file_pos, out_stream=sys.stdout):
    """Read newly appended content from serial.txt and write directly to stream."""
    if not os.path.exists(serial_path):
        return file_pos
    try:
        with open(serial_path, 'r', errors='replace') as f:
            f.seek(file_pos)
            data = f.read()
            if data:
                out_stream.write(data)
                out_stream.flush()
                return f.tell()
    except Exception:
        pass
    return file_pos


def main():
    ap = argparse.ArgumentParser(
        description="Boot a SWEETXNU payload in QEMU's virt board.",
        epilog="Examples:\n"
               "  qemu-sweet.py payload.bin out/\n"
               "  qemu-sweet.py payload.bin out/ --live-serial\n"
               "  qemu-sweet.py payload.bin out/ --kernel kernel.macho --fail-on-panic\n"
               "  qemu-sweet.py --check-env\n",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument('payload', nargs='?', help='SWEETXNU payload binary (from mkpayload.py)')
    ap.add_argument('outdir', nargs='?', help='Output directory for fb.png, regs.txt, and logs')
    ap.add_argument('--secs', type=float, default=20, help='Duration to run QEMU in seconds (default: 20)')
    ap.add_argument('--gdb', action='store_true', help='start halted, gdb stub on :1234, no timeout')
    ap.add_argument('--hmp', action='append', default=[], help='extra monitor command run after stop (repeatable), e.g. "x /8gx 0xfffffe..."')
    ap.add_argument('--cpu', default='cortex-a76', help='QEMU CPU model (default: cortex-a76)')
    ap.add_argument('--gic-version', type=int, choices=(2, 3), help='must match the ADT (default: infer)')
    ap.add_argument('--cpus', type=int, choices=range(1, 9), help='must match the ADT (default: infer)')
    ap.add_argument('--boot-args-line-length', type=int, choices=(608, 1024), default=1024,
                    help='kernel boot_args ABI: 1024 for XNU26 (default), 608 for XNU8796')
    ap.add_argument('--kernel', help='matching unstripped kernel, for exact framebuffer text decoding (uses llvm-nm)')
    ap.add_argument('--fail-on-panic', action='store_true', help='exit 2 for a visible panic, 3 for incomplete/blank decoding; requires --kernel')
    ap.add_argument('--live-serial', action='store_true', help='stream serial console output to stdout in real time')
    ap.add_argument('--check-env', action='store_true', help='check QEMU installation and dependencies without running a payload')
    a = ap.parse_args()

    if a.check_env:
        ok = check_qemu_dependency(a.cpu)
        if shutil.which('llvm-nm'):
            print('[*] llvm-nm found in PATH', file=sys.stderr)
        else:
            print('[!] Optional: llvm-nm not found (needed only when using --kernel for text decoding)', file=sys.stderr)
        return 0 if ok else 1

    if not a.payload or not a.outdir:
        ap.error('payload and outdir are required unless --check-env is specified')

    if a.fail_on_panic and not a.kernel:
        ap.error('--fail-on-panic requires --kernel')

    if not check_qemu_dependency(a.cpu):
        return 1

    if a.kernel and not check_kernel_tools(a.kernel):
        return 1

    payload = open(a.payload, 'rb').read()
    try:
        gic_version, cpus = qemu_config(payload)
        if a.gic_version is not None and a.gic_version != gic_version:
            raise ValueError('--gic-version differs from the payload ADT')
        if a.cpus is not None and a.cpus != cpus:
            raise ValueError('--cpus differs from the payload ADT')
        fb = framebuffer(payload)
        phys_base, img, ba_pa, entry_pa = build_image(payload, a.boot_args_line_length)
        if phys_base < STUB_PA + PAGE:
            raise ValueError('kernel RAM overlaps the QEMU boot stub/DTB')
    except (ValueError, TypeError, struct.error, zlib.error) as e:
        ap.error(str(e))
    font = None
    if a.kernel:
        symbols = subprocess.check_output(['llvm-nm', '--defined-only', a.kernel], text=True)
        fonts = [int(line.split()[0], 16) for line in symbols.splitlines()
                 if line.split()[-1:] == ['_iso_font']]
        if len(fonts) != 1:
            ap.error('kernel must contain exactly one _iso_font symbol')
        font = payload_data(payload, fonts[0], 256 * 16)
    os.makedirs(a.outdir, exist_ok=True)
    out = os.path.abspath(a.outdir)

    open(f'{out}/ram.bin', 'wb').write(img)
    open(f'{out}/stub.bin', 'wb').write(stub(ba_pa, entry_pa))
    print(f'phys_base={phys_base:#x} boot_args={ba_pa:#x} entry={entry_pa:#x}', file=sys.stderr)

    mon = f'{out}/mon.sock'
    if os.path.exists(mon):
        os.unlink(mon)
    # virt RAM starts at 0x40000000: cover the payload's RAM (mkadt.py --highmem needs 8G).
    mem_size = struct.unpack_from('<Q', payload, 0x40)[0]
    ram_end = max(phys_base + mem_size, fb.base + fb.size)
    ram_gib = max(4, -(-(ram_end - 0x40000000) // (1 << 30)))
    cmd = ['qemu-system-aarch64', '-M', f'virt,gic-version={gic_version}', '-cpu', a.cpu, '-smp', str(cpus), '-m', f'{ram_gib}G',
           '-nographic', '-serial', f'file:{out}/serial.txt', '-qmp', f'unix:{mon},server,nowait',
           '-d', 'int,guest_errors,unimp', '-D', f'{out}/qemu.log', '-no-reboot',
           '-device', f'loader,file={out}/ram.bin,addr={phys_base:#x},force-raw=on',
           '-device', f'loader,file={out}/stub.bin,addr={STUB_PA:#x},force-raw=on',
           '-device', f'loader,addr={STUB_PA:#x},cpu-num=0']
    if a.gdb:
        cmd += ['-S', '-gdb', 'tcp::1234']
    open(f'{out}/command.json', 'w').write(json.dumps(cmd, indent=2) + '\n')

    p = None
    serial_file = f'{out}/serial.txt'
    serial_pos = 0

    try:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL)
        if a.gdb:
            print('gdb stub on :1234; QMP socket:', mon, file=sys.stderr)
            p.wait()
            return 0

        t0 = time.time()
        while p.poll() is None and time.time() - t0 < a.secs:
            if a.live_serial:
                serial_pos = stream_serial_file(serial_file, serial_pos)
            time.sleep(0.2)

        if a.live_serial:
            stream_serial_file(serial_file, serial_pos)

        result = {'status': 'qemu_exited', 'qemu_returncode': p.returncode}
        if p.poll() is None:
            commands = ['stop', 'info cpus']
            replies = qmp(mon, *commands)
            snapshots = [f'\n(qemu) {c}\n{r}' for c, r in zip(commands, replies)]
            for cpu in range(cpus):
                regs = qmp(mon, 'info registers', cpu_index=cpu)[0]
                snapshots.append(f'\n(qemu cpu {cpu}) info registers\n{regs}')
            replies = qmp(mon, *a.hmp, pmemsave=(fb.base, fb.size, f'{out}/fb.raw'))
            snapshots += [f'\n(qemu) {c}\n{r}' for c, r in zip(a.hmp, replies)]
            open(f'{out}/regs.txt', 'w').write(''.join(snapshots))
            p.kill()
            p.wait()
            raw = open(f'{out}/fb.raw', 'rb').read()
            write_png(f'{out}/fb.png', raw, fb)
            result = {'status': 'not_decoded'}
            if font is not None:
                screen, unknown = decode_screen(raw, font, fb)
                open(f'{out}/fb.txt', 'w').write(screen)
                result = screen_result(screen, unknown)
            os.unlink(f'{out}/fb.raw')
        else:
            print(f'qemu exited early: {p.returncode}', file=sys.stderr)

        result.update(gic_version=gic_version, cpus=cpus, framebuffer=fb._asdict())
        open(f'{out}/result.json', 'w').write(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result), file=sys.stderr)
        print(f'{out}/fb.png  {out}/regs.txt  {out}/qemu.log', file=sys.stderr)
        if a.fail_on_panic and result['status'] == 'panic':
            return 2
        if a.fail_on_panic and result['status'] in ('incomplete_decode', 'blank_screen'):
            return 3
        if result['status'] == 'qemu_exited':
            return 1
        return 0

    except KeyboardInterrupt:
        print('\n[!] Interrupted by user, stopping QEMU...', file=sys.stderr)
        if p and p.poll() is None:
            p.kill()
            p.wait()
        return 130
    finally:
        if p and p.poll() is None:
            p.kill()
            p.wait()
        ram_file = f'{out}/ram.bin'
        if os.path.exists(ram_file):
            try:
                os.unlink(ram_file)
            except Exception:
                pass


if __name__ == '__main__':
    sys.exit(main())
