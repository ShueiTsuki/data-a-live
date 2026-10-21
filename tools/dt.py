"""Small flattened-device-tree reader for looking things up in pandora-live.dtb.

    from dt import load
    root = load('device/pandora-live.dtb')
    for n in root.find(compatible='arm,gic-v3'): print(n.path, n.reg())
"""
import struct


class Node:
    def __init__(self, name, parent):
        self.name, self.parent = name, parent
        self.props, self.children = {}, []

    @property
    def path(self):
        if self.parent is None:
            return '/'
        p = self.parent.path
        return (p if p != '/' else '') + '/' + self.name

    def str(self, key, default=None):
        v = self.props.get(key)
        if v is None:
            return default
        return v.rstrip(b'\0').decode(errors='replace').split('\0')

    def u32(self, key):
        v = self.props.get(key)
        return None if v is None else list(struct.unpack('>%dI' % (len(v) // 4), v))

    def cells(self, key):
        p = self.parent
        while p is not None and '#address-cells' not in p.props:
            p = p.parent
        return p

    def reg(self):
        """List of (address, size) using the parent's #address-cells/#size-cells."""
        v = self.u32('reg')
        if v is None:
            return []
        p = self.parent
        ac = (p.u32('#address-cells') or [2])[0] if p else 2
        sc = (p.u32('#size-cells') or [1])[0] if p else 1
        out, step = [], ac + sc
        for i in range(0, len(v) - step + 1, step):
            a = 0
            for w in v[i:i + ac]:
                a = (a << 32) | w
            s = 0
            for w in v[i + ac:i + step]:
                s = (s << 32) | w
            out.append((a, s))
        return out

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()

    def find(self, compatible=None, name=None, prop=None):
        for n in self.walk():
            if compatible and compatible not in (n.str('compatible') or []):
                continue
            if name and not (n.name == name or n.name.startswith(name + '@')):
                continue
            if prop and prop not in n.props:
                continue
            yield n

    def child(self, name):
        for c in self.children:
            if c.name == name or c.name.split('@')[0] == name:
                return c
        return None


def load(path):
    data = open(path, 'rb').read()
    magic, total, off_struct, off_strings = struct.unpack('>IIII', data[:16])
    assert magic == 0xd00dfeed
    strings = data[off_strings:]
    pos, cur, root = off_struct, None, None
    while True:
        (tok,) = struct.unpack('>I', data[pos:pos + 4]); pos += 4
        if tok == 1:
            end = data.index(b'\0', pos)
            n = Node(data[pos:end].decode(), cur)
            if cur is None:
                root = n
            else:
                cur.children.append(n)
            cur = n
            pos = (end + 4) & ~3
        elif tok == 2:
            cur = cur.parent
        elif tok == 3:
            ln, off = struct.unpack('>II', data[pos:pos + 8]); pos += 8
            name = strings[off:strings.index(b'\0', off)].decode()
            cur.props[name] = data[pos:pos + ln]
            pos = (pos + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            break
    phandles = {}
    for n in root.walk():
        ph = n.u32('phandle')
        if ph:
            phandles[ph[0]] = n
    root.phandles = phandles
    return root
