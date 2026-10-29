#!/usr/bin/env python3
"""Repack a starlte legacy Samsung boot image with Enxerto and a SWEETXNU payload.

  mkboot-starlte.py TEMPLATE_BOOT_IMG ENXERTO_IMAGE PAYLOAD OUT_IMG

The supplied Lineage starlte template uses the pre-versioned Android header:
the word at offset 40 is dt_size, NOT header_version. A separate DTBH v2
section follows the kernel/ramdisk/second sections. Keep the template's
addresses, OS version, command line, second stage, DTBH and Samsung footer.
Only kernel/ramdisk sizes and the SHA-1 image ID change in the header.

This creates a file; it does not flash or boot a device. Hardware handoff
requirements and the observed template are documented in docs/STARLTE.md.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


SECTIONS = ('kernel', 'ramdisk', 'second', 'dtb')
SIZE_OFFSETS = (8, 16, 24, 40)
FOOTER = b'SEANDROIDENFORCE'


def image_id(parts):
    digest = hashlib.sha1()
    for name in SECTIONS:
        data = parts[name]
        digest.update(data)
        digest.update(struct.pack('<I', len(data)))
    return digest.digest().ljust(32, b'\0')


def dtbh_entries(data):
    if len(data) < 12 or data[:4] != b'DTBH':
        raise ValueError('template needs a separate Samsung DTBH section')
    version, count = struct.unpack_from('<II', data, 4)
    table_end = 12 + 32 * count
    if version != 2 or not 1 <= count <= 100 or table_end > len(data):
        raise ValueError('invalid DTBH v2 table')
    entries = []
    for i in range(count):
        chip, platform, subtype, first, last, offset, size, delimiter = struct.unpack_from('<8I', data, 12 + 32 * i)
        if chip != 9810 or first > last or delimiter != 0x20:
            raise ValueError(f'DTBH entry {i} is not an Exynos9810 revision range')
        if offset < table_end or size < 40 or offset + size > len(data):
            raise ValueError(f'DTBH entry {i} is out of bounds')
        magic, total = struct.unpack_from('>II', data, offset)
        if magic != 0xd00dfeed or not 40 <= total <= size:
            raise ValueError(f'DTBH entry {i} has an invalid FDT header')
        entries.append(dict(chip=chip, platform=platform, subtype=subtype,
                            hw_rev=first, hw_rev_end=last, offset=offset, size=size,
                            fdt_size=total))
    return entries


def unpack(data):
    if len(data) < 1632 or data[:8] != b'ANDROID!':
        raise ValueError('not a legacy Android boot image')
    page, = struct.unpack_from('<I', data, 36)
    if page < 1632 or page & (page - 1) or page > len(data):
        raise ValueError('invalid boot-image page size')
    parts, padding, offsets = {}, {}, {}
    offset = page
    for name, field in zip(SECTIONS, SIZE_OFFSETS):
        size, = struct.unpack_from('<I', data, field)
        end = offset + size
        next_page = (end + page - 1) & -page
        if next_page > len(data):
            raise ValueError(f'truncated {name} section')
        offsets[name] = offset
        parts[name] = data[offset:end]
        padding[name] = data[end:next_page]
        offset = next_page
    if data[offset:] != FOOTER:
        raise ValueError('expected exactly the Samsung SEANDROIDENFORCE footer')
    entries = dtbh_entries(parts['dtb'])
    if image_id(parts) != data[576:608]:
        raise ValueError('template SHA-1 image ID does not match its sections')
    return dict(header=data[:page], page_size=page, parts=parts,
                padding=padding, offsets=offsets, dtbh=entries)


def repack(template, image, payload):
    parts = {**template['parts'], 'kernel': image, 'ramdisk': payload}
    header = bytearray(template['header'])
    for name, field in zip(SECTIONS, SIZE_OFFSETS):
        struct.pack_into('<I', header, field, len(parts[name]))
    header[576:608] = image_id(parts)
    result = header
    page = template['page_size']
    for name in SECTIONS:
        data = parts[name]
        result += data
        # Unchanged-length sections keep their exact original padding, so
        # repacking the original components is a byte-identical roundtrip.
        if len(data) == len(template['parts'][name]):
            result += template['padding'][name]
        else:
            result += b'\0' * (-len(data) % page)
    return bytes(result) + FOOTER


def build(template_data, image, payload):
    template = unpack(template_data)
    if repack(template, template['parts']['kernel'], template['parts']['ramdisk']) != template_data:
        raise ValueError('template does not roundtrip byte-for-byte')
    if len(image) < 64 or image[56:60] != b'ARM\x64':
        raise ValueError('Enxerto must be a raw ARM64 Image')
    if struct.unpack_from('<Q', image, 16)[0] != len(image):
        raise ValueError('Enxerto Image must be padded to its image_size (BSS/stacks)')
    if len(payload) < 0x2a8 or payload[:8] != b'SWEETXNU':
        raise ValueError('ramdisk must be a raw SWEETXNU payload from mkpayload.py')
    version, count = struct.unpack_from('<II', payload, 8)
    stride = 40 if version == 2 else 32
    if version not in (1, 2) or not 1 <= count <= 64 or 0x2a8 + stride * count > len(payload):
        raise ValueError('invalid SWEETXNU payload table')
    result = repack(template, image, payload)
    check = unpack(result)
    if check['parts'] != {**template['parts'], 'kernel': image, 'ramdisk': payload}:
        raise ValueError('repacked sections do not match inputs')
    info = dict(template_sha256=hashlib.sha256(template_data).hexdigest(),
                sha256=hashlib.sha256(result).hexdigest(), bytes=len(result),
                format='Samsung legacy Android boot + DTBH v2',
                page_size=check['page_size'], dtbh=check['dtbh'],
                footer=FOOTER.decode(), template_roundtrip='identical',
                image_id=result[576:596].hex(),
                sections={name: dict(offset=check['offsets'][name], bytes=len(data),
                                     sha256=hashlib.sha256(data).hexdigest())
                          for name, data in check['parts'].items()})
    return result, info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('template', type=Path)
    parser.add_argument('image', type=Path)
    parser.add_argument('payload', type=Path)
    parser.add_argument('out', type=Path)
    args = parser.parse_args()
    try:
        result, info = build(args.template.read_bytes(), args.image.read_bytes(), args.payload.read_bytes())
        # Keep the supplied template and earlier test images intact.
        with args.out.open('xb') as out:
            out.write(result)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    print(json.dumps({'out': str(args.out), **info}, indent=2))


if __name__ == '__main__':
    main()
