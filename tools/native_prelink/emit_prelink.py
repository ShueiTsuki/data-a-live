#!/usr/bin/env python3
"""Construct an experimental native prelink candidate; no runtime qualification."""
import argparse
import base64
import copy
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

from native_prelink import backed_offset, exports, macho, plan, unserialize_xml

# Standard SHA-256 round constants, stored as 64 little-endian words. Some
# assemblers embed these data in executable sections; do not decode as code.
SHA256_K_DIGEST = '74ef7306e7452d6859b6463ce496b8df30925f69e1b2969e1f3f34bbc9c6af04'
VECTOR_REPEAT_DIGEST = 'f7bb30778915b574b63a17573babfdeb97251ae92fb57e7302e69783f454f92a'
ADLER32_VECTOR_DIGEST = '48bdf8e57f8647f2ef3cfda5b500209c4be9b5742632db0ff3614aca8d7ee63d'
MATH_LITERAL_POOLS = (
    ('00000000004a87c0f039fafe422e8640', 2160, '1a5eb119b57d453a6123eb839b259b96f2ff4f687fc8aabfdcf965ded106df81'),
    ('0000807f0000f042db0f493f00008039', 1248, '92c7b2f87685a1e7a92cad6e7d408fdbc03bd67d1207b6579214040f413b2161'),
    ('1f2003d500000000001018c00ee52615', 2188, 'c103ce1f2e204bbbfcc2069de79758b0ea399e5e7b39b2b8337cbf3d3f7f960a'),
    ('0000c1c00000807fff799f501344d33f', 1088, '22b9e038ad1cbdef1265a71f9213354287637a561eaa52f9ec25f90d10bd0a7c'),
    ('0000c1c00000807f0000000084aec68f', 212, '43ef8598a507444b21d8a2abbe4065554baaa3b6a29dbe390465cc0ba0dfbe28'),
    ('9b1a86a049faa8bd053f4e7b9dee213e', 56, 'e874aeba59eebdebdf68d18b1196d1ca6aeee12b110985a856f8fe404e0c76ee'),
)
COALESCED = {'__PRELINK_TEXT': '__TEXT', '__PLK_TEXT_EXEC': '__TEXT_EXEC',
             '__PLK_DATA_CONST': '__DATA_CONST', '__PRELINK_DATA': '__DATA'}


def align(value):
    return (value + 4095) & ~4095


def to_xml(value):
    if isinstance(value, dict):
        node = ET.Element('dict')
        for key, child in value.items():
            ET.SubElement(node, 'key').text = key
            node.append(to_xml(child))
    elif isinstance(value, list):
        node = ET.Element('array'); node.extend(to_xml(child) for child in value)
    elif isinstance(value, bool):
        node = ET.Element('true' if value else 'false')
    elif isinstance(value, int):
        if not 0 <= value < 1 << 64:
            raise ValueError('Metadata integer outside 64-bit range')
        node = ET.Element('integer', {'size': '64'}); node.text = hex(value)
    elif isinstance(value, bytes):
        node = ET.Element('data'); node.text = base64.b64encode(value).decode()
    elif isinstance(value, str):
        node = ET.Element('string'); node.text = value
    else:
        raise ValueError('Unsupported metadata value')
    return node


def pc_relative_target(word, pc):
    def signed(value, bits):
        return value - (1 << bits) if value & (1 << (bits - 1)) else value
    if word & 0x9f000000 in (0x10000000, 0x90000000):
        immediate = signed(((word >> 5) & 0x7ffff) << 2 | ((word >> 29) & 3), 21)
        page = bool(word & 0x80000000)
        return ((pc & ~4095) + (immediate << 12), True) if page else (pc + immediate, False)
    if word & 0x7c000000 == 0x14000000:
        return pc + (signed(word & 0x3ffffff, 26) << 2), False
    if word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000 or word & 0x3b000000 == 0x18000000:
        return pc + (signed((word >> 5) & 0x7ffff, 19) << 2), False
    if word & 0x7e000000 == 0x36000000:
        return pc + (signed((word >> 5) & 0x3fff, 14) << 2), False
    return None


def instruction_inventory(original_data, parent, modules):
    extents = [s for module in modules.values() for s in module['segments'] if s['size'] and s['name'] != '__LINKEDIT']
    literal_ranges, instruction_count = [], 0
    for identifier, module in modules.items():
        for segment in module['segments']:
            if segment['name'] != '__TEXT_EXEC' or not segment['filesize']:
                continue
            at = backed_offset(parent, segment['va'], segment['filesize'], len(original_data))
            blob = original_data[at:at + segment['filesize']]
            literals = []
            # Complete identity is required: a prefix alone never excludes code.
            for prefix, size, digest, prefix_offset in (
                    (bytes.fromhex('982f8a42'), 256, SHA256_K_DIGEST, 0),
                    (bytes.fromhex('0001000100010001'), 528, VECTOR_REPEAT_DIGEST, 64),
                    (bytes(range(32, 24, -1)), 40, ADLER32_VECTOR_DIGEST, 0),
                    *((bytes.fromhex(prefix), size, digest, 0) for prefix, size, digest in MATH_LITERAL_POOLS)):
                search = 0
                while (search := blob.find(prefix, search)) >= 0:
                    start = search - prefix_offset
                    if start >= 0 and start % 4 == 0 and hashlib.sha256(blob[start:start + size]).hexdigest() == digest:
                        literals.append((start, start + size))
                        literal_ranges.append({'module': identifier, 'va': segment['va'] + start,
                                               'size': size, 'sha256': digest})
                    search += 4
            for offset in range(0, len(blob) - 3, 4):
                if any(start <= offset < end for start, end in literals):
                    continue
                word, = struct.unpack_from('<I', blob, offset)
                decoded = pc_relative_target(word, segment['va'] + offset)
                if decoded is None:
                    continue
                target, page = decoded
                if page:
                    internal = any((s['va'] & ~4095) <= target < align(s['va'] + s['size']) for s in extents)
                else:
                    internal = any(s['va'] <= target < s['va'] + s['size'] for s in extents)
                if not internal:
                    raise ValueError(f'External PC-relative encoding at {segment["va"] + offset:#x}; no instruction rewrite supported')
                instruction_count += 1
    return {'pc_relative_encodings_preserved': instruction_count, 'known_inline_data': literal_ranges,
            'register_dataflow_qualified': False}


def emit(native_data, original_data, roots, slide, early_security_init=None):
    report = plan(native_data, original_data, roots, slide)
    native, original = macho(native_data), macho(original_data)
    marker = [address for address, names in exports(native_data, native).items()
              if '_kalloc_sparse_prelink_index_version' in names]
    if len(marker) != 1:
        raise ValueError('Native kernel lacks sparse-prelink index ABI')
    marker_at = backed_offset(native['segments'], marker[0], 4, len(native_data))
    if struct.unpack_from('<I', native_data, marker_at)[0] != 1:
        raise ValueError('Unsupported native sparse-prelink index ABI')
    parent = original['segments']
    old_info = next(s for s in parent if s['name'] == '__PRELINK_INFO')
    at = backed_offset(parent, old_info['va'], old_info['filesize'], len(original_data))
    metadata = unserialize_xml(original_data[at:at + old_info['filesize']])
    entries = {e['CFBundleIdentifier']: e for e in metadata['_PrelinkInfoDictionary']}
    modules = {}
    for identifier in report['components']:
        va = entries[identifier]['_PrelinkExecutableLoadAddr']
        modules[identifier] = macho(original_data, backed_offset(parent, va, 32, len(original_data)))
    report.update(instruction_inventory(original_data, parent, modules))
    early_security_init = early_security_init or {}
    if early_security_init:
        markers = [address for address, names in exports(native_data, native).items()
                   if '_native_prelink_security_init_version' in names]
        if len(markers) != 1:
            raise ValueError('Native kernel lacks early security initializer ABI')
        at = backed_offset(native['segments'], markers[0], 4, len(native_data))
        if struct.unpack_from('<I', native_data, at)[0] != 1:
            raise ValueError('Unsupported early security initializer ABI')
    report['early_security_initializers'] = {}
    for identifier, address in early_security_init.items():
        if identifier not in modules or entries[identifier].get('AppleSecurityExtension') is not True:
            raise ValueError('Early initializer requires a selected security extension')
        if address % 4 or not any(s['name'] == '__TEXT_EXEC' and s['initprot'] == 5
                and not s['maxprot'] & 2 and s['va'] <= address
                and address + 4 <= s['va'] + s['filesize'] for s in modules[identifier]['segments']):
            raise ValueError('Early security initializer outside original module code')
        if not 0 <= address + slide < 1 << 64:
            raise ValueError('Early initializer address overflow')
        report['early_security_initializers'][identifier] = address + slide
    output = bytearray(native_data)
    new_segments = []

    def append_segment(name, va, blob, size, protection):
        if va % 4096 or size % 4096 or len(blob) > size:
            raise ValueError('Coalesced segment must have native 4K alignment')
        if any(s['size'] and va < s['va'] + s['size'] and s['va'] < va + size
               for s in [*native['segments'], *new_segments]):
            raise ValueError('Coalesced segment collides with existing mapping')
        placeholder = next((s for s in native['segments'] if s['name'] == name), None)
        if placeholder is None or placeholder['size'] or len(placeholder['sections']) != 1:
            raise ValueError(f'Expected one empty native {name} segment/section')
        fileoff = align(len(output))
        output.extend(bytes(fileoff - len(output)))
        output.extend(blob)
        output.extend(bytes(size - len(blob)))
        command = placeholder['command_offset']
        struct.pack_into('<4Q2I', output, command + 24, va, size, fileoff, size, protection, protection)
        section = placeholder['sections'][0]['command_offset']
        struct.pack_into('<QQI', output, section + 32, va, size, fileoff)
        result = {'name': name, 'va': va, 'size': size, 'filesize': size, 'fileoff': fileoff}
        new_segments.append(result)
        return result

    # Keep coalesced geometry, but only copy selected component segments.
    # Packed kernel-resource records are copied as original readonly metadata.
    for coalesced_name, component_name in COALESCED.items():
        source = next(s for s in parent if s['name'] == coalesced_name)
        blob = bytearray(source['size'])
        for module in modules.values():
            for segment in module['segments']:
                if segment['name'] != component_name or not segment['filesize']:
                    continue
                begin = segment['va'] - source['va']
                if begin < 0 or begin + segment['size'] > len(blob):
                    raise ValueError('Component is outside original coalesced range')
                fileoff = backed_offset(parent, segment['va'], segment['filesize'], len(original_data))
                blob[begin:begin + segment['filesize']] = original_data[fileoff:fileoff + segment['filesize']]
        if coalesced_name == '__PRELINK_TEXT':
            for identifier in report['kernel_interfaces']:
                entry = entries[identifier]
                va, length = entry['_PrelinkExecutableLoadAddr'], entry['_PrelinkExecutableSize']
                begin = va - source['va']
                at = backed_offset(parent, va, length, len(original_data))
                if begin < 0 or begin + length > len(blob):
                    raise ValueError('Kernel interface outside readonly prelink range')
                blob[begin:begin + length] = original_data[at:at + length]
        append_segment(coalesced_name, source['va'] + slide, blob, source['size'], source['initprot'])

    def new_offset(address, length):
        return backed_offset(new_segments, address, length, len(output))

    for binding in report['bindings']:
        struct.pack_into('<Q', output, new_offset(binding['new_site'], 8), binding['new_target'])
    # Mach-O VM addresses are not all included in the cache's pointer list.
    # Rebase these headers explicitly; preserve section flags and offsets.
    for identifier, module in modules.items():
        old_header = entries[identifier]['_PrelinkExecutableLoadAddr']
        original_header = backed_offset(parent, old_header, 32, len(original_data))
        new_header = new_offset(old_header + slide, 32)
        for segment in module['segments']:
            if segment['va']:
                at = new_header + segment['command_offset'] - original_header
                struct.pack_into('<Q', output, at + 24, segment['va'] + slide)
            for section in segment['sections']:
                if section['va']:
                    at = new_header + section['command_offset'] - original_header
                    struct.pack_into('<Q', output, at + 32, section['va'] + slide)
    report['module_entrypoints'] = {}
    for identifier in report['components']:
        va = entries[identifier]['_PrelinkKmodInfo']
        at = backed_offset(parent, va, 196, len(original_data))
        start, stop = struct.unpack_from('<QQ', original_data, at + 180)
        # The original OSKext boot path slides this field separately from its
        # coalesced pointer table. An ordinary relink must do the same.
        address, = struct.unpack_from('<Q', original_data, at + 156)
        if address:
            if address != entries[identifier]['_PrelinkExecutableLoadAddr']:
                raise ValueError('Original kmod address disagrees with its component header')
            struct.pack_into('<Q', output, new_offset(va + slide + 156, 8), address + slide)
        report['module_entrypoints'][identifier] = {'kmod_info_va': va + slide,
            'start_va': start + slide if start else 0, 'stop_va': stop + slide if stop else 0}
    headers = sorted(entries[name]['_PrelinkExecutableLoadAddr'] + slide for name in report['components'])
    prelink_text = next(s for s in new_segments if s['name'] == '__PRELINK_TEXT')
    index_size = len(headers) * 8
    index_va = (prelink_text['va'] + prelink_text['size'] - index_size) & ~7
    occupied_end = max([s['va'] + slide + s['size'] for module in modules.values()
                        for s in module['segments'] if s['name'] == '__TEXT'] +
                       [entries[name]['_PrelinkExecutableLoadAddr'] + slide + entries[name]['_PrelinkExecutableSize']
                        for name in report['kernel_interfaces']])
    if index_va < occupied_end:
        raise ValueError('No readonly prelink padding for the sparse header index')
    index_at = new_offset(index_va, index_size)
    struct.pack_into(f'<{len(headers)}Q', output, index_at, *headers)
    report['sparse_index'] = {'va': index_va, 'size': index_size, 'fileoff': index_at, 'headers': headers, 'abi': 1}
    selected_entries = []
    for identifier in [*report['kernel_interfaces'], *report['components'], *report['codeless_components']]:
        entry = copy.deepcopy(entries[identifier])
        if '_NativePrelinkEarlySecurityInit' in entry:
            raise ValueError('Refuse an initializer supplied by input metadata')
        if identifier in early_security_init:
            entry['_NativePrelinkEarlySecurityInit'] = early_security_init[identifier] + slide
        for key in ('_PrelinkExecutableLoadAddr', '_PrelinkExecutableSourceAddr', '_PrelinkKmodInfo'):
            if entry.get(key):
                entry[key] += slide
        selected_entries.append(entry)
    new_base = next(s['va'] for s in new_segments if s['name'] == '__PRELINK_TEXT')
    offsets = [b['new_site'] - new_base for b in report['bindings']]
    if any(not 0 <= offset <= 0xffffffff for offset in offsets):
        raise ValueError('Rebased KASLR offset cannot fit original 32-bit format')
    new_metadata = {'_PrelinkInfoDictionary': selected_entries,
                    '_PrelinkLinkKASLROffsets': struct.pack(f'<{len(offsets) + 1}I', len(offsets), *offsets),
                    '_PrelinkKCID': hashlib.sha256(native_data + original_data).digest()[:16]}
    xml = ET.tostring(to_xml(new_metadata), encoding='utf-8').replace(b' />', b'/>') + b'\0'
    info_va = align(max(s['va'] + s['size'] for s in native['segments'] if s['size']))
    append_segment('__PRELINK_INFO', info_va, xml, align(len(xml)), 3)
    # Expand the existing command in verified Mach-O header padding; mapped
    # payload offsets and all original kernel/component instruction bytes stay put.
    commands = []
    for command, at, length in native['commands']:
        blob = bytearray(output[at:at + length])
        if command == 0x19 and blob[8:24].rstrip(b'\0') == b'__PRELINK_TEXT':
            struct.pack_into('<I', blob, 4, length + 80)
            struct.pack_into('<I', blob, 64, 2)
            struct.pack_into('<Q', blob, 72 + 40, index_va - prelink_text['va'])
            blob.extend(struct.pack('<16s16sQQ8I', b'__macho_list', b'__PRELINK_TEXT',
                                   index_va, index_size, index_at, 3, 0, 0, 0, 0, 0, 0))
        commands.append(blob)
    rebuilt = b''.join(commands)
    first_payload = min(section['fileoff'] for segment in native['segments']
                        for section in segment['sections'] if section['size'] and section['fileoff'])
    if 32 + len(rebuilt) > first_payload:
        raise ValueError('Insufficient native Mach-O header padding for sparse index section')
    struct.pack_into('<I', output, 20, len(rebuilt))
    output[32:32 + len(rebuilt)] = rebuilt
    # No executable instruction byte is patched, including original POST code.
    text_hashes = {}
    for identifier, module in modules.items():
        for segment in module['segments']:
            if segment['name'] == '__TEXT_EXEC' and segment['filesize']:
                old = backed_offset(parent, segment['va'], segment['filesize'], len(original_data))
                new = new_offset(segment['va'] + slide, segment['filesize'])
                if output[new:new + segment['filesize']] != original_data[old:old + segment['filesize']]:
                    raise ValueError('Original executable bytes changed')
                text_hashes[identifier] = hashlib.sha256(original_data[old:old + segment['filesize']]).hexdigest()
    macho(output)
    report.update(candidate_sha256=hashlib.sha256(output).hexdigest(), coalesced_segments=new_segments,
                  original_executable_sha256=text_hashes, candidate_bytes=len(output),
                  source_inputs_modified=False, runtime_qualified=False, loadable=False)
    return bytes(output), report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--prelinked', required=True, type=Path)
    parser.add_argument('--root', required=True, action='append')
    parser.add_argument('--slide', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--early-security-init', action='append', default=[], metavar='BUNDLE=ORIGINAL_VA',
                        help='Explicit original void(void) policy initializer, called before lockdown; requires source ABI')
    args = parser.parse_args()
    initializers = {}
    for request in args.early_security_init:
        identifier, separator, address = request.partition('=')
        if not separator or not identifier or identifier in initializers:
            parser.error('Use one BUNDLE=ORIGINAL_VA initializer per module')
        initializers[identifier] = int(address, 0)
    image, report = emit(args.kernel.read_bytes(), args.prelinked.read_bytes(), args.root, args.slide, initializers)
    if args.output.exists():
        raise SystemExit('Refuse to overwrite an existing candidate')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Experimental candidate: {len(image)} bytes; executable bytes unchanged; runtime unqualified')


if __name__ == '__main__':
    main()
