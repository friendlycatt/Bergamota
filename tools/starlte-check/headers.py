#!/usr/bin/env python3
"""Compile complete XNU translation units using native doconf and real headers.

Needs a prepared XNU12377 tree, clang, bison, flex, tcsh and unifdef. --cpp
also builds Apple's migcom; DriverKit IIG outputs/Apple Clang may still be
required. This is a compile check, not a replacement for the macOS build.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
COMPONENTS = ('osfmk', 'iokit', 'pexpert', 'bsd', 'libkern', 'libsa')
SOURCES = ('osfmk/arm64/genassym.c', 'osfmk/arm64/arm_vm_init.c',
           'pexpert/arm/pe_fiq.c', 'osfmk/arm64/sweet_early.c', 'iokit/KernelConfigTables.cpp')


def run(command, log, **kwargs):
    command = [str(arg) for arg in command]
    print(shlex.join(command), file=log, flush=True)
    return subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, **kwargs)


def configure(xnu, out, tcsh, unifdef):
    tools = out / 'tools'
    tools.mkdir(parents=True, exist_ok=True)
    source = xnu / 'SETUP/config'
    with (out / 'configure.log').open('w') as log:
        run(['bison', '-y', '-d', '-o', tools / 'parser.c', source / 'parser.y'], log)
        run(['flex', '--header-file=' + str(tools / 'lexer.yy.h'),
             '-o', tools / 'lexer.yy.c', source / 'lexer.l'], log)
        names = ('externs', 'main', 'mkheaders', 'mkioconf', 'mkmakefile', 'openp', 'searchp')
        run(['clang', '-std=gnu11', '-DYY_NO_INPUT', '-I' + str(source), '-I' + str(tools),
             *[source / (name + '.c') for name in names], tools / 'lexer.yy.c',
             tools / 'parser.c', '-o', tools / 'config'], log)
        dest = out / 'SETUP/config/config'
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            dest.symlink_to(tools / 'config')
        # doconf invokes unifdef by name.
        bindir = out / 'bin'
        bindir.mkdir(exist_ok=True)
        tool = bindir / 'unifdef'
        if tool.is_symlink():
            tool.unlink()
        if not tool.exists():
            tool.symlink_to(unifdef)
        env = dict(os.environ, OBJROOT=str(out), SRCROOT=str(xnu),
                   PATH=str(bindir) + os.pathsep + os.environ['PATH'])
        for component in COMPONENTS:
            config = out / component / 'DEVELOPMENT'
            config.mkdir(parents=True, exist_ok=True)
            source = xnu / component / 'conf'
            env['SOURCE'] = str(source)
            run([tcsh, '-f', xnu / 'SETUP/config/doconf', '-c', '-cpu', 'arm64',
                 '-soc', 'sweet', '-platform', 'MacOSX', '-d', config, '-s', source,
                 '-m', xnu / 'config', 'DEVELOPMENT'], log, env=env)
            assert (config / 'Makefile').is_file(), component
    # Both directories export their source headers into san/.
    exports = out / 'exports/san'
    exports.mkdir(parents=True, exist_ok=True)
    for folder in ('memory', 'coverage'):
        for header in (xnu / 'san' / folder).glob('*.h'):
            dest = exports / header.name
            if dest.is_symlink():
                dest.unlink()
            if not dest.exists():
                dest.symlink_to(header)
    print('PASS native doconf: DEVELOPMENT/ARM64/SWEET, six components')


def flags(xnu, out, component):
    config = out / component / 'DEVELOPMENT'
    makefile = (config / 'Makefile').read_text()
    definitions = shlex.split(re.search(r'^export CONFIG_DEFINES = (.*)$', makefile, re.M)[1])
    result = ['--target=arm64-apple-macos26', '-march=armv8.2-a+crypto', '-nostdlibinc',
              '-ffreestanding', '-fno-builtin', '-mkernel', '-fblocks', '-fno-strict-aliasing',
              '-DAPPLE', '-DKERNEL', '-DKERNEL_PRIVATE', '-DXNU_KERNEL_PRIVATE', '-DPRIVATE',
              '-D__MACHO__=1', '-Dvolatile=__volatile', '-DXNU_KERN_EVENT_DATA_IS_VLA',
              '-DCURRENT_MACHINE_CONFIG_LC=sweet', '-DARM64_BOARD_CONFIG_SWEET',
              '-Darm64', '-DARM64', '-D__ARM64__', '-DLP64', '-DPAGE_SIZE_FIXED',
              '-DVM_KERNEL_LINK_ADDRESS=0xfffffff007004000', '-DARM_LARGE_MEMORY=1',
              '-D__API__=v4', '-DXNU_TARGET_OS_OSX', '-DXNU_PLATFORM_MacOSX',
              '-include', str(config / 'meta_features.h'), *definitions]
    order = (component, *[c for c in COMPONENTS if c != component])
    result += ['-I' + str(out / c / 'DEVELOPMENT') for c in order]
    result += ['-I' + str(xnu / c) for c in (*order, 'EXTERNAL_HEADERS', 'osfmk/libsa')]
    result += ['-I' + str(xnu), '-I' + str(out / 'exports')]
    if component == 'osfmk':
        result += ['-DMACH_KERNEL_PRIVATE', '-DMACH_KERNEL']
    elif component == 'iokit':
        result += ['-DDRIVER_PRIVATE', '-DIOKIT_KERNEL_PRIVATE', '-DDRIVERKIT_PRIVATE=1',
                   '-DIOMATCHDEBUG=1', '-DIOALLOCDEBUG=1', '-DPRIVATE_WIFI_ONLY=1', '-DIOASSERT=1']
    elif component == 'pexpert':
        result += ['-DPEXPERT_KERNEL_PRIVATE']
    return result


def compile_source(xnu, out, source, dest=None):
    component = source.split('/')[0]
    cpp = source.endswith('.cpp')
    genassym = Path(source).stem == 'genassym'
    dest = dest or out / (Path(source).stem + ('.s' if genassym else '.o'))
    lang = ['-std=gnu++2b', '-fsized-deallocation', '-fapple-kext'] if cpp else ['-std=gnu17']
    command = ['clang++' if cpp else 'clang', *flags(xnu, out, component), *lang,
               *(['-fno-integrated-as', '-S'] if genassym else ['-c']),
               '-O2', str(xnu / source), '-o', str(dest)]
    log = dest.with_suffix('.compile.log')
    with log.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    errors = [line for line in log.read_text().splitlines() if 'error:' in line]
    record = dict(source=source, sha256=hashlib.sha256((xnu / source).read_bytes()).hexdigest(),
                  command=command, returncode=result.returncode, errors=errors)
    print(('FAIL' if result.returncode else 'PASS') + ' real headers: ' + source)
    for error in errors[:3]:
        print(' ', error)
    return record


def genassym_values(path):
    values = {name: int(value) for name, value in re.findall(
        r'DEFINITION__define__(\w+):\s*\.ascii\s+"[#$]?(-?\d+)"', path.read_text())}
    assert len(values) > 100, f'{path}: incomplete genassym assembly'
    return values


def generate_mig(xnu, out, source):
    """Build current Apple MIG on Linux; use only real .defs for target headers."""
    build = out / 'mig-build'
    build.mkdir(exist_ok=True)
    src = source / 'migcom.tproj'
    commands = []
    with (build / 'host-build.log').open('w') as log:
        run(['bison', '-y', '-d', '--defines=' + str(build / 'y.tab.h'),
             '-o', build / 'y.tab.c', src / 'parser.y'], log)
        run(['flex', '-o', build / 'lex.yy.c', src / 'lexxer.l'], log)
        # handler.c is historical unused code, absent from the Xcode Sources phase.
        run(['clang', '-std=gnu11', '-O2', '-DMIG_VERSION="starlte-header-check"',
             '-include', HERE / 'mig_host.h', '-I' + str(src), '-I' + str(build),
             '-I' + str(xnu / 'osfmk'), '-I' + str(xnu / 'libkern'),
             '-idirafter', xnu / 'bsd', '-idirafter', xnu / 'EXTERNAL_HEADERS',
             *[p for p in sorted(src.glob('*.c')) if p.name != 'handler.c'],
             build / 'lex.yy.c', build / 'y.tab.c', '-o', build / 'migcom'], log)
        makefile = (xnu / 'osfmk/mach/Makefile').read_text()
        exports = out / 'exports/mach'
        exports.mkdir(parents=True, exist_ok=True)
        for group in ('MIG_UUHDRS', 'MIG_USHDRS'):
            names = re.search('^' + group + r' = \\\n((?:[^\n]+\\\n)*[^\n]+)',
                              makefile, re.M)[1].replace('\\', '').split()
            for name in names:
                server = group == 'MIG_USHDRS'
                base = name.removesuffix('_server.h' if server else '.h')
                cpp = ['clang', '--target=arm64-apple-macos26', '-E', '-x', 'c',
                       '-D__MACH30__', '-DAPPLE', '-DKERNEL', '-DKERNEL_PRIVATE',
                       '-DXNU_KERNEL_PRIVATE', '-DPRIVATE', '-D__MACHO__=1',
                       '-DARM64_BOARD_CONFIG_SWEET', '-I' + str(xnu / 'osfmk'),
                       '-I' + str(xnu / 'bsd'), '-I' + str(xnu / 'EXTERNAL_HEADERS'),
                       str(xnu / 'osfmk/mach' / (base + '.defs'))]
                preprocessed = subprocess.run(cpp, check=True, capture_output=True, text=True).stdout
                (build / (base + '.defs.i')).write_text(preprocessed)
                gen = [str(build / 'migcom'), '-novouchers', '-user', '/dev/null',
                       '-server', '/dev/null', '-header', '/dev/null' if server else str(exports / name)]
                if server:
                    gen += ['-sheader', str(exports / name)]
                run(gen, log, input=preprocessed, text=True)
                commands.append(dict(header=name, cpp=cpp, generate=gen))
    (out / 'mig-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    print(f'PASS native migcom: {len(commands)} exported Mach headers')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu', type=Path, default=ROOT / 'research/starlte/xnu')
    ap.add_argument('--out', type=Path, default=ROOT / 'research/starlte/header-check')
    ap.add_argument('--tcsh', type=Path, default=shutil.which('tcsh') or
                    ROOT / 'research/starlte/header-check/tcsh-build/tcsh')
    ap.add_argument('--unifdef', type=Path, default=shutil.which('unifdef') or ROOT / 'research/tools/unifdef/unifdef')
    ap.add_argument('--cpp', action='store_true', help='also generate MIG headers and check both IOKit drivers')
    ap.add_argument('--mig-source', type=Path, default=ROOT / 'research/tools/bootstrap_cmds')
    args = ap.parse_args()
    xnu, out = args.xnu.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    configure(xnu, out, args.tcsh.resolve(), args.unifdef.resolve())
    sources = list(SOURCES)
    if args.cpp:
        generate_mig(xnu, out, args.mig_source.resolve())
        sources += ['iokit/Kernel/arm/SweetCPU.cpp', 'iokit/Kernel/arm/SweetPlatformExpert.cpp']
    results = [compile_source(xnu, out, source) for source in sources]
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    if not results[0]['returncode']:
        (out / 'genassym.json').write_text(json.dumps(genassym_values(out / 'genassym.s'), indent=2) + '\n')
    raise SystemExit(int(any(result['returncode'] for result in results)))


if __name__ == '__main__':
    main()
