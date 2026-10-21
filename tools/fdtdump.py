"""Dump a flattened device tree (.dtb) as text, dtc-style.

Usage: python fdtdump.py IN.dtb [OUT.dts]
"""
import struct
import sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def fmt_value(name, val):
    if not val:
        return None
    # Printable NUL-terminated string list
    if val[-1] == 0 and all(32 <= c < 127 or c == 0 for c in val) and b'\0\0' not in val and val[0] != 0:
        return ', '.join('"%s"' % s for s in val[:-1].decode().split('\0'))
    if len(val) % 4 == 0:
        return '<' + ' '.join('0x%x' % w for w in struct.unpack('>%dI' % (len(val) // 4), val)) + '>'
    return '[' + ' '.join('%02x' % c for c in val) + ']'


def dump(data):
    magic, total, off_struct, off_strings = struct.unpack('>IIII', data[:16])
    assert magic == 0xd00dfeed, 'not a dtb'
    strings = data[off_strings:]
    pos, depth, out = off_struct, 0, []
    while True:
        (tok,) = struct.unpack('>I', data[pos:pos + 4]); pos += 4
        if tok == FDT_BEGIN_NODE:
            end = data.index(b'\0', pos)
            name = data[pos:end].decode() or '/'
            pos = (end + 4) & ~3
            out.append('\t' * depth + name + ' {')
            depth += 1
        elif tok == FDT_END_NODE:
            depth -= 1
            out.append('\t' * depth + '};')
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack('>II', data[pos:pos + 8]); pos += 8
            val = data[pos:pos + ln]; pos = (pos + ln + 3) & ~3
            pname = strings[nameoff:strings.index(b'\0', nameoff)].decode()
            v = fmt_value(pname, val)
            out.append('\t' * depth + pname + (' = ' + v if v is not None else '') + ';')
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            raise ValueError('bad token %d at %d' % (tok, pos - 4))
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    text = dump(open(sys.argv[1], 'rb').read())
    if len(sys.argv) > 2:
        open(sys.argv[2], 'w', encoding='utf-8').write(text)
    else:
        sys.stdout.write(text)
