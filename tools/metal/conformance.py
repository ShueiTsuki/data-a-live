"""conformance.py: the Metal methods the plugin's objects lack, public API first (run in WSL).

Two references for each kind of Metal object (device, command buffer, render encoder, texture, ...):
- public: every iOS-available method of its protocols in the SDK's Metal headers. Any app may call these, so all of
  them are needed whether or not the system uses them today;
- internal: the rest of what a real iPhone X answers, i.e. the instance methods of the A11 driver's class and its
  superclasses (AGXMetalA11, IOGPU, Metal's own base classes) and of the protocols they adopt. Only the ones the
  system references are listed.
The plugin's side is its compiled classes (llvm-objdump's ObjC metadata) plus what it installs at run time on the
device (NBDeviceMethods, the runtime's answer selectors, Apple's _MTLDevice base class).

Who references a method: the selector references (__objc_selrefs) of every image in the dyld shared cache (Metal's
own implementation images left out) and, with --root (the mounted iOS system volume), of the programs outside the
cache. A referenced name may be sent to some other object; names Foundation, CoreFoundation or libobjc also implement
are marked "generic".

Output: OUT/conformance.md and OUT/conformance.json; the class dumps are kept in OUT/dumps.

  python3 tools/metal/conformance.py --dsc DSC --ipsw IPSW --plugin BUNDLE_BINARY --nm-src NATIVEMETAL \\
      --m2v-src METAL2VULKAN --sdk SDK/System/Library/Frameworks/Metal.framework/Headers [--root SYSTEM_VOLUME] \\
      --out ref/metal_conformance
"""
import argparse
import collections
import glob
import json
import mmap
import os
import re
import struct
import subprocess

DUMPS = {
    'metal': '/System/Library/Frameworks/Metal.framework/Metal',
    'iogpu': '/System/Library/PrivateFrameworks/IOGPU.framework/IOGPU',
    'agx': '/System/Library/Extensions/AGXMetalA11.bundle/AGXMetalA11',
    'iosurface': '/System/Library/Frameworks/IOSurface.framework/IOSurface',
    'foundation': '/System/Library/Frameworks/Foundation.framework/Foundation',
    'corefoundation': '/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation',
    'objc': '/usr/lib/libobjc.A.dylib',
}
GENERIC = ('foundation', 'corefoundation', 'objc')

# kind, the A11 driver's class (or Metal's own where Metal makes the object), the plugin's class (None: not made),
# the public protocol (None: Apple-internal only)
KINDS = [
    ('device', 'AGXA11Device', 'NativeBridgeMetalDevice', 'MTLDevice'),
    ('command queue', 'AGXA11FamilyCommandQueue', 'NBQueue', 'MTLCommandQueue'),
    ('command buffer', 'AGXA11FamilyCommandBuffer', 'NBCommandBuffer', 'MTLCommandBuffer'),
    ('render encoder', 'AGXA11FamilyRenderContext', 'NBRenderEncoder', 'MTLRenderCommandEncoder'),
    ('compute encoder', 'AGXA11FamilyComputeContext', 'NBComputeEncoder', 'MTLComputeCommandEncoder'),
    ('blit encoder', 'AGXA11FamilyBlitContext', 'NBBlitEncoder', 'MTLBlitCommandEncoder'),
    ('texture', 'AGXA11FamilyTexture', 'NBTexture', 'MTLTexture'),
    ('buffer', 'AGXA11FamilyBuffer', 'NBBuffer', 'MTLBuffer'),
    ('heap', 'AGXA11FamilyHeap', 'NBHeap', 'MTLHeap'),
    ('library', '_MTLLibrary', 'NBLibrary', 'MTLLibrary'),
    ('function', '_MTLFunctionInternal', 'NBFunction', 'MTLFunction'),
    ('render pipeline', 'AGXA11FamilyRenderPipeline', 'NBRenderPipeline', 'MTLRenderPipelineState'),
    ('compute pipeline', 'AGXA11FamilyComputePipeline', 'NBComputePipeline', 'MTLComputePipelineState'),
    ('sampler', 'AGXA11FamilySampler', 'NBSampler', 'MTLSamplerState'),
    ('depth stencil', 'AGXA11FamilyDepthStencilState', 'NBDepthStencil', 'MTLDepthStencilState'),
    ('event', '_IOGPUMetalMTLEvent', 'NBEvent', 'MTLEvent'),
    ('shared event', '_MTLSharedEvent', 'NBSharedEvent', 'MTLSharedEvent'),
    ('fence', 'IOGPUMetalFence', 'NBFence', 'MTLFence'),
    ('binary archive', 'AGXA11FamilyBinaryArchive', 'NBBinaryArchive', 'MTLBinaryArchive'),
    ('parallel render encoder', 'AGXA11FamilyThreadedRenderPass', None, 'MTLParallelRenderCommandEncoder'),
    ('argument encoder', 'AGXA11FamilyIndirectArgumentEncoder', None, 'MTLArgumentEncoder'),
    ('indirect command buffer', 'AGXA11FamilyIndirectCommandBuffer', None, 'MTLIndirectCommandBuffer'),
    ('indirect render command', 'AGXA11FamilyIndirectRenderCommand', None, 'MTLIndirectRenderCommand'),
    ('indirect compute command', 'AGXA11FamilyIndirectComputeCommand', None, 'MTLIndirectComputeCommand'),
    ('rasterization rate map', 'AGXA11FamilyRasterizationRateMap', None, 'MTLRasterizationRateMap'),
    ('counter sample buffer', 'AGXMTLCounterSampleBuffer', None, 'MTLCounterSampleBuffer'),
    ('resource state encoder', 'AGXA11FamilyResourceStateContext', None, 'MTLResourceStateCommandEncoder'),
    ('dynamic library', 'AGXA11FamilyDynamicLibrary', None, 'MTLDynamicLibrary'),
    ('function handle', 'AGXA11FamilyFunctionHandle', None, 'MTLFunctionHandle'),
    ('visible function table', 'AGXA11FamilyVisibleFunctionTable', None, 'MTLVisibleFunctionTable'),
    ('intersection function table', 'IOGPUMetalIntersectionFunctionTable', None, 'MTLIntersectionFunctionTable'),
    ('acceleration structure', 'AGXA11FamilyRayTracingAccelerationStructure', None, 'MTLAccelerationStructure'),
    ('acceleration structure encoder', 'AGXA11FamilyRayTracingGPUBuilder', None,
     'MTLAccelerationStructureCommandEncoder'),
    ('IO command queue', 'IOGPUMetalIOCommandQueue', None, 'MTLIOCommandQueue'),
    ('IO command buffer', 'IOGPUMetalIOCommandBuffer', None, 'MTLIOCommandBuffer'),
    ('IO file handle', 'IOGPUMetalIOHandleRaw', None, 'MTLIOFileHandle'),
    ('resource group', 'AGXA11FamilyResourceGroup', 'NBResourceGroup', None),
    ('late eval event', 'AGXA11FamilyLateEvalEvent', None, None),
    ('texture layout', 'AGXTextureLayout', None, None),
    ('pipeline library', '_MTLPipelineLibrary', None, None),
]
# Images that implement Metal rather than use it: their references say nothing about what clients send.
IMPLEMENTERS = re.compile(r'/Metal\.framework/|IOGPU|/AGX|MetalTools|GPUTools|MTLCompiler|MetalSerializer')
# Libraries named first in the "who" column when they reference a selector.
KEY = ['RenderBox', 'QuartzCore', 'MetalPerformanceShaders', 'MPSCore', 'MPSImage', 'MPSNeuralNetwork', 'MPSMatrix',
       'CoreImage', 'SwiftUI', 'UIKitCore', 'MetalKit', 'VideoToolbox', 'AVFoundation', 'MediaToolbox', 'CoreMedia',
       'PhotosUI', 'PhotosUICore', 'PhotoLibraryServices', 'ImageIO', 'Vision', 'Espresso', 'CoreML', 'SceneKit',
       'SpriteKit', 'ARKit', 'PosterKit', 'WallpaperKit', 'ChronoKit', 'WidgetKit', 'CoreAnimation', 'VectorKit',
       'WebCore', 'JavaScriptCore', 'Celestial', 'CameraUI', 'AVFCapture']

METHOD = re.compile(r'^([-+])\[(\S+)\s+(\S.*)\];$')
PROPERTY = re.compile(r'^@property \(([^)]*)\) (\w+);')


def class_dump(ipsw, dsc, image, path):
    if not os.path.exists(path):
        with open(path, 'w') as f:
            subprocess.run([ipsw, 'class-dump', dsc, image], stdout=f, stderr=subprocess.DEVNULL, check=True)
    return open(path, encoding='utf-8', errors='replace').read().splitlines()


def property_methods(attrs, name):
    getter, setter, readonly = name, 'set%s%s:' % (name[:1].upper(), name[1:]), False
    for a in re.findall(r'(?:"[^"]*"|[^,])+', attrs):
        if a == 'R':
            readonly = True
        elif a.startswith('G'):
            getter = a[1:]
        elif a.startswith('S'):
            setter = a[1:]
    return [getter] if readonly else [getter, setter]


def parse_dump(lines, classes, protocols):
    """ipsw class-dump text: classes (super, protocols, instance methods, categories merged) and protocols."""
    cur = None
    for line in lines:
        m = re.match(r'^@interface (\w+) \((\w*)\)', line)
        if m:
            cur = classes.setdefault(m.group(1), {'super': None, 'protos': set(), 'methods': set()})
            continue
        m = re.match(r'^@interface (\w+)(?: : (\w+))?(?: <([^>]*)>)?', line)
        if m:
            cur = classes.setdefault(m.group(1), {'super': None, 'protos': set(), 'methods': set()})
            cur['super'] = m.group(2) or cur['super']
            if m.group(3):
                cur['protos'] |= {p.strip() for p in m.group(3).split(',')}
            continue
        m = re.match(r'^@protocol (\w+)(?: <([^>]*)>)?', line)
        if m:
            cur = protocols.setdefault(m.group(1), {'parents': set(), 'methods': set()})
            if m.group(2):
                cur['parents'] |= {p.strip() for p in m.group(2).split(',')}
            continue
        if line.startswith('@end'):
            cur = None
            continue
        if cur is None:
            continue
        m = METHOD.match(line)
        if m and m.group(1) == '-':
            cur['methods'].add(m.group(3).strip())
            continue
        m = PROPERTY.match(line)
        if m and 'parents' in cur:   # a protocol's properties are its methods; a class lists its accessors
            cur['methods'].update(property_methods(m.group(1), m.group(2)))


def parse_objdump(text):
    """llvm-objdump --objc-meta-data: each class's superclass and instance methods (categories included)."""
    supers, methods = {}, collections.defaultdict(set)
    header, owner, meta, collecting = None, None, False, False
    for line in text.splitlines():
        if line.startswith('Contents of') and '__objc_protolist' in line:
            break
        m = re.match(r'^[0-9a-f]{16} 0x[0-9a-f]+ _OBJC_CLASS_\$_(\w+)$', line)
        if m:
            header = m.group(1)
            continue
        m = re.match(r'^ {4}superclass 0x[0-9a-f]+ _OBJC_CLASS_\$_(\w+)$', line)
        if m and header:
            supers[header] = m.group(1)
            header = None
            continue
        if re.match(r'^\s+flags 0x', line):
            meta = 'RO_META' in line
            continue
        m = re.match(r'^ {21}name 0x[0-9a-f]+ (\S+)$', line)
        if m:
            owner = m.group(1)
            continue
        m = re.match(r'^\s+(baseMethods|instanceMethods|classMethods) 0x([0-9a-f]+)', line)
        if m:
            collecting = m.group(1) != 'classMethods' and not meta and int(m.group(2), 16) != 0
            continue
        if re.match(r'^\s+(baseProtocols|ivars|weakIvarLayout|baseProperties|instanceProperties|protocols) ', line):
            collecting = False
            continue
        m = re.match(r'^\t\t\s+name 0x[0-9a-f]+ (\S+)$', line)
        if m and collecting and owner:
            methods[owner].add(m.group(1))
    return supers, methods


def chain_methods(classes, protocols, name):
    """Instance methods of class NAME and its superclasses up to NSObject, and of every protocol they adopt."""
    own, declared, seen = set(), set(), set()

    def proto(p):
        if p in seen or p not in protocols:
            return
        seen.add(p)
        declared.update(protocols[p]['methods'])
        for q in protocols[p]['parents']:
            proto(q)
    c = name
    while c and c != 'NSObject' and c in classes:
        own |= classes[c]['methods']
        for p in classes[c]['protos']:
            proto(p)
        c = classes[c]['super']
    return own, declared, seen


def strip_macro(text, names):
    """TEXT without each NAME(...) macro call (balanced parentheses)."""
    out, i = [], 0
    pat = re.compile(r'\b(%s)\s*\(' % '|'.join(names))
    while True:
        m = pat.search(text, i)
        if not m:
            return ''.join(out) + text[i:]
        out.append(text[i:m.start()])
        depth, j = 1, m.end()
        while j < len(text) and depth:
            depth += {'(': 1, ')': -1}.get(text[j], 0)
            j += 1
        i = j


MACROS = ['API_AVAILABLE', 'API_UNAVAILABLE', 'API_DEPRECATED', 'API_DEPRECATED_WITH_REPLACEMENT', 'NS_SWIFT_NAME',
          'NS_SWIFT_UNAVAILABLE', 'NS_AVAILABLE', 'NS_AVAILABLE_IOS', 'NS_DEPRECATED', 'NS_DEPRECATED_IOS', '__attribute__',
          'MTL_DEPRECATED', 'API_UNAVAILABLE_BEGIN', 'NS_SWIFT_ASYNC_NAME', 'NS_SWIFT_ASYNC']


def unavailable_on_ios(decl):
    return any('ios' in m.group(1) for m in re.finditer(r'API_UNAVAILABLE\(([^)]*)\)', decl))


def sdk_protocols(headers):
    """The public protocols of the SDK's Metal headers: name -> {'parents', 'methods'}, iOS-available methods only."""
    protocols = {}
    for path in sorted(glob.glob(os.path.join(headers, '*.h'))):
        text = open(path, encoding='utf-8', errors='replace').read()
        text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
        text = re.sub(r'//[^\n]*', ' ', text)
        text = re.sub(r'@protocol\s+[\w\s,]+;', ' ', text)   # forward declarations
        for m in re.finditer(r'@protocol\s+(\w+)\s*(<[^>]*>)?(.*?)@end', text, flags=re.S):
            name, parents, body = m.groups()
            # The protocol's own availability is the macros right before it (back to the previous declaration).
            pre = text[max(0, m.start() - 400):m.start()]
            pre = pre[max(pre.rfind(';'), pre.rfind('@end'), pre.rfind('}')) + 1:]
            if unavailable_on_ios(pre):
                continue
            entry = protocols.setdefault(name, {'parents': set(), 'methods': set()})
            if parents:
                entry['parents'] |= {p.strip() for p in parents.strip('<>').split(',')}
            body = re.sub(r'@(optional|required)', ';', body)
            for decl in body.split(';'):
                decl = ' '.join(decl.split())
                if not decl or unavailable_on_ios(decl):
                    continue
                decl = strip_macro(decl, MACROS)
                decl = re.sub(r'\b(NS_RETURNS_RETAINED|NS_REFINED_FOR_SWIFT|MTL_EXPORT|NS_REQUIRES_SUPER)\b', '', decl).strip()
                if decl.startswith('-'):
                    rest = decl[1:].strip()
                    if rest.startswith('('):
                        depth, j = 0, 0
                        for j, ch in enumerate(rest):
                            depth += {'(': 1, ')': -1}.get(ch, 0)
                            if depth == 0:
                                break
                        rest = rest[j + 1:].strip()
                    labels = re.findall(r'(\w+)\s*:\s*\(', rest)
                    sel = ''.join(l + ':' for l in labels) if labels else re.match(r'\w+', rest).group(0)
                    entry['methods'].add(sel)
                elif decl.startswith('@property'):
                    am = re.match(r'@property\s*\(([^)]*)\)', decl)
                    attrs = [a.strip() for a in am.group(1).split(',')] if am else []
                    name_ = re.findall(r'(\w+)\s*$', decl)
                    if not name_:
                        continue
                    prop = name_[0]
                    getter = next((a.split('=')[1].strip() for a in attrs if a.startswith('getter')), prop)
                    entry['methods'].add(getter)
                    if 'readonly' not in attrs:
                        entry['methods'].add(next((a.split('=')[1].strip() for a in attrs if a.startswith('setter')),
                                                  'set%s%s:' % (prop[:1].upper(), prop[1:])))
    return protocols


def public_methods(sdk, names):
    """Methods of the public protocols NAMES and their public parents (NSObject's left out)."""
    out, seen, todo = set(), set(), list(names)
    while todo:
        p = todo.pop()
        if p in seen or p not in sdk:
            continue
        seen.add(p)
        out |= sdk[p]['methods']
        todo += list(sdk[p]['parents'])
    return out


def device_answers(nm_src, m2v_src):
    names = set()
    for path in glob.glob(os.path.join(m2v_src, 'crates', '*', 'src', 'private_properties.rs')):
        names |= set(re.findall(r'\(\s*"([A-Za-z0-9_:]+)"\s*,\s*\[', open(path).read()))
    text = open(os.path.join(nm_src, 'bridge', 'NativeMetalObjects.rs')).read()
    block = text[text.find('const PUBLIC_ANSWERS'):]
    block = block[:block.find('];')]
    names |= set(re.findall(r'\("([A-Za-z0-9_:]+)",\s*\d+\)', block))
    return names


class Cache:
    """A dyld shared cache with its subcaches: virtual address reads."""

    def __init__(self, path):
        self.maps, self.files = [], []
        for p in [path] + sorted(glob.glob(path + '.*')):
            if p.endswith(('.map', '.symbols', '.a2s', '.cache')):
                continue
            f = open(p, 'rb')
            mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if mm[:7] != b'dyld_v1':
                continue
            self.files.append(mm)
            moff, mcount = struct.unpack_from('<II', mm, 0x10)
            for i in range(mcount):
                addr, size, foff, _, _ = struct.unpack_from('<QQQII', mm, moff + 32 * i)
                self.maps.append((addr, size, foff, mm))
        self.main = self.files[0]

    def read(self, va, n):
        for addr, size, foff, mm in self.maps:
            if addr <= va < addr + size:
                return mm[foff + va - addr:foff + va - addr + n]
        return None

    def cstr(self, va):
        for addr, size, foff, mm in self.maps:
            if addr <= va < addr + size:
                o = foff + va - addr
                return mm[o:mm.find(b'\0', o, o + 1024)].decode('latin-1')
        return None

    def images(self):
        mm = self.main
        off, count = struct.unpack_from('<II', mm, 0x18)
        if not off:
            off, count = struct.unpack_from('<II', mm, 0x1c0)
        for i in range(count):
            addr, _, _, path_off, _ = struct.unpack_from('<QQQII', mm, off + 32 * i)
            yield addr, mm[path_off:mm.find(b'\0', path_off)].decode()


def selrefs(cache, addr):
    """Selector names an image references (its __objc_selrefs)."""
    hdr = cache.read(addr, 32)
    if not hdr or struct.unpack_from('<I', hdr)[0] != 0xfeedfacf:
        return set()
    ncmds, sizeofcmds = struct.unpack_from('<II', hdr, 16)
    cmds = cache.read(addr + 32, sizeofcmds)
    out, o = set(), 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', cmds, o)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', cmds, o + 64)[0]
            for s in range(nsects):
                so = o + 72 + 80 * s
                sect = cmds[so:so + 16].rstrip(b'\0')
                if sect == b'__objc_selrefs':
                    saddr, ssize = struct.unpack_from('<QQ', cmds, so + 32)
                    raw = cache.read(saddr, ssize) or b''
                    for (p,) in struct.iter_unpack('<Q', raw):
                        name = cache.cstr(p & 0xFFFFFFFFFF)
                        if name:
                            out.add(name)
        o += size
    return out


def file_selrefs(path):
    """Selector names a Mach-O file outside the cache references (the arm64 slice of a fat file). Its selrefs are
    chained-fixup rebases: the low 36 bits are the target's address or its offset from the image base."""
    with open(path, 'rb') as f:
        data = f.read()
    base = 0
    magic = struct.unpack_from('<I', data)[0] if len(data) >= 4 else 0
    if magic == 0xbebafeca:   # fat, big-endian header
        n = struct.unpack_from('>I', data, 4)[0]
        slices = [struct.unpack_from('>IIIII', data, 8 + 20 * i) for i in range(n)]
        pick = [s for s in slices if s[0] == 0x0100000c and (s[1] & 0xff) == 0] or slices
        base = pick[0][2]
    if len(data) < base + 32 or struct.unpack_from('<I', data, base)[0] != 0xfeedfacf:
        return set()
    ncmds = struct.unpack_from('<I', data, base + 16)[0]
    o, sects, text = base + 32, {}, 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, o)
        if cmd == 0x19:
            seg = data[o + 8:o + 24].rstrip(b'\0')
            if seg == b'__TEXT':
                text = struct.unpack_from('<Q', data, o + 24)[0]
            nsects = struct.unpack_from('<I', data, o + 64)[0]
            for s in range(nsects):
                so = o + 72 + 80 * s
                name = data[so:so + 16].rstrip(b'\0')
                addr, ssize, soff = struct.unpack_from('<QQI', data, so + 32)
                sects[name] = (addr, ssize, soff)
        o += size
    if b'__objc_selrefs' not in sects or b'__objc_methname' not in sects:
        return set()
    maddr, msize, moff = sects[b'__objc_methname']
    saddr, ssize, soff = sects[b'__objc_selrefs']
    out = set()
    for (p,) in struct.iter_unpack('<Q', data[base + soff:base + soff + ssize]):
        for t in (p & 0xFFFFFFFFF, (p & 0xFFFFFFFFF) + text):
            if maddr <= t < maddr + msize:
                at = base + moff + t - maddr
                out.add(data[at:data.find(b'\0', at)].decode('latin-1'))
                break
    return out


def root_files(root):
    skip = ('/System/Library/Caches', '/usr/share', '/System/Library/Fonts', '/System/Library/LinguisticData',
            '/System/Library/PreinstalledAssets', '/System/Library/Audio', '/System/Library/Pearl',
            '/private/var', '/System/Library/AssetsV2', '/System/Library/Templates')
    for d, dirs, files in os.walk(root):
        rel = d[len(root):] or '/'
        if rel.startswith(skip):
            dirs[:] = []
            continue
        for name in files:
            p = os.path.join(d, name)
            if '.' in name and not name.endswith(('.dylib',)):
                continue   # executables have no extension; bundles' binaries share the bundle's base name
            try:
                if os.path.islink(p) or os.path.getsize(p) < 4096:
                    continue
                with open(p, 'rb') as f:
                    if f.read(4) not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
                        continue
            except OSError:
                continue
            yield p, rel


def main():
    ap = argparse.ArgumentParser()
    for a in ('--dsc', '--ipsw', '--plugin', '--nm-src', '--m2v-src', '--sdk', '--out'):
        ap.add_argument(a, required=True)
    ap.add_argument('--root', help='the mounted iOS system volume, for the programs outside the dyld cache')
    args = ap.parse_args()
    os.makedirs(os.path.join(args.out, 'dumps'), exist_ok=True)

    classes, protocols, generic, nsobject = {}, {}, set(), set()
    for key, image in DUMPS.items():
        lines = class_dump(args.ipsw, args.dsc, image, os.path.join(args.out, 'dumps', key + '.txt'))
        if key in GENERIC:
            c, p = {}, {}
            parse_dump(lines, c, p)
            for v in c.values():
                generic |= v['methods']
            for v in p.values():
                generic |= v['methods']
            # Both sides are NSObjects: its methods (Foundation's categories on it included) are answered everywhere.
            nsobject |= c.get('NSObject', {}).get('methods', set()) | p.get('NSObject', {}).get('methods', set())
        else:
            parse_dump(lines, classes, protocols)

    meta = subprocess.run(['llvm-objdump', '--macho', '--objc-meta-data', args.plugin], capture_output=True, text=True).stdout
    nb_super, nb_methods = parse_objdump(meta)

    def ours(name):
        got, c = set(), name
        while c and c != 'NSObject':
            got |= nb_methods.get(c, set())
            c = nb_super.get(c)
        return got
    device_own = nb_methods.get('NBDeviceMethods', set()) | device_answers(args.nm_src, args.m2v_src)
    device_base, _, _ = chain_methods(classes, protocols, '_MTLDevice')

    users = collections.defaultdict(list)
    cache = Cache(args.dsc)
    images = 0
    for addr, path in cache.images():
        if IMPLEMENTERS.search(path):
            continue
        images += 1
        for s in selrefs(cache, addr):
            users[s].append(os.path.basename(path))
    programs = 0
    if args.root:
        for path, rel in root_files(args.root):
            try:
                refs = file_selrefs(path)
            except (OSError, struct.error):
                continue
            if refs:
                programs += 1
                for s in refs:
                    users[s].append(os.path.basename(path))

    def who(sel):
        names = users.get(sel, [])
        key = [k for k in KEY if k in names]
        rest = sorted(n for n in names if n not in key)
        return key + rest

    sdk = sdk_protocols(args.sdk)
    report = {'images': images, 'programs': programs, 'kinds': []}
    for kind, real, mine, public_name in KINDS:
        own, declared, seen = chain_methods(classes, protocols, real)
        public = public_methods(sdk, ([public_name] if public_name else []) + sorted(seen)) - nsobject
        internal = (own | declared) - public - nsobject
        if mine == 'NativeBridgeMetalDevice':
            have = device_own | device_base
        else:
            have = ours(mine) if mine else set()
        order = lambda s: (-len(users.get(s, [])), s)
        entry = {'kind': kind, 'real': real, 'plugin': mine, 'protocol': public_name, 'public': len(public),
                 'public_have': len(public & have), 'public_missing': [], 'internal_missing': [], 'base_only': []}
        for s in sorted(public - have, key=order):
            entry['public_missing'].append({'selector': s, 'users': len(users.get(s, [])), 'who': who(s)})
        for s in sorted(internal - have, key=order):
            if users.get(s):
                entry['internal_missing'].append({'selector': s, 'driver': s in own, 'generic': s in generic,
                                                  'users': len(users[s]), 'who': who(s)})
        if mine == 'NativeBridgeMetalDevice':
            for s in sorted(((public | internal) & device_base) - device_own, key=order):
                entry['base_only'].append({'selector': s, 'public': s in public, 'users': len(users.get(s, [])),
                                           'who': who(s)})
        report['kinds'].append(entry)

    with open(os.path.join(args.out, 'conformance.json'), 'w') as f:
        json.dump(report, f, indent=1)
    write_md(report, os.path.join(args.out, 'conformance.md'))


def write_md(report, path):
    out = ['# Metal 对照表', '',
           '公开接口：iOS 16.1 SDK 的 Metal 头文件里 iOS 能用的方法。第三方应用按这些写，必须全部有。',
           '内部接口：iPhone X（A11）驱动的类和父类、以及它们采纳的协议里其余的方法。只列系统库引用到的。',
           '引用者：dyld 缓存里 %d 个映像（Metal 自身的实现映像不算）和系统卷上 %d 个程序的选择子引用。' % (
               report['images'], report['programs']),
           '一个库引用了某个方法名，不一定是发给这类对象的；“通用名”是 Foundation、CoreFoundation 或 libobjc 也有的名字。', '',
           '| 对象 | 公开协议 | 我们的类 | 公开方法 | 我们有 | 缺 | 内部方法缺且被引用 |', '|---|---|---|---|---|---|---|']
    for k in report['kinds']:
        out.append('| %s | %s | %s | %d | %d | %d | %d |' % (k['kind'], k['protocol'] or '（内部）', k['plugin'] or '没有',
                                                         k['public'], k['public_have'], len(k['public_missing']),
                                                         len(k['internal_missing'])))
    for k in report['kinds']:
        out += ['', '## %s（%s，苹果 %s，我们 %s）' % (k['kind'], k['protocol'] or '内部', k['real'], k['plugin'] or '没有')]
        if k['public_missing']:
            out += ['', '公开接口缺 %d 个：' % len(k['public_missing']), '', '| 方法 | 引用数 | 引用者（前 8 个） |', '|---|---|---|']
            for m in k['public_missing']:
                out.append('| `%s` | %d | %s |' % (m['selector'], m['users'], ', '.join(m['who'][:8])))
        else:
            out += ['', '公开接口齐全。']
        if k['internal_missing']:
            out += ['', '内部接口缺且被引用 %d 个：' % len(k['internal_missing']), '', '| 方法 | 引用数 | 引用者（前 8 个） |',
                    '|---|---|---|']
            for m in k['internal_missing']:
                out.append('| `%s`%s | %d | %s |' % (m['selector'], '（通用名）' if m['generic'] else '', m['users'],
                                                   ', '.join(m['who'][:8])))
        if k['base_only']:
            out += ['', '只靠苹果 _MTLDevice 基类、插件自己没写的 %d 个（公开的和被引用的列出）：' % len(k['base_only']), '']
            for m in k['base_only']:
                if m['public'] or m['users']:
                    out.append('- `%s`%s（%d）：%s' % (m['selector'], '，公开' if m['public'] else '', m['users'],
                                                     ', '.join(m['who'][:8])))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
