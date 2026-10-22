#!/usr/bin/env python3
"""Build the Apple Device Tree for the PANDORA xnu (Xiaomi 17 Pro, SM8850).

Derived from ios-sweet tools/mkadt.py (dipa-studio/ios-sweet 425b2510). Phone values come from
the 17 Pro's live FDT (device/DT_FINDINGS.md) and shim/hello measurements.
Usage:  mkadt_pandora.py OUT.adt [--dump] [--qemu]
  --qemu   QEMU virt twin: QEMU's GIC/timer, RAM at 0xb0000000 and ios-sweet's 1080x2400
           framebuffer at 0x9c000000 so tools/qemu-sweet.py can decode the screen.
  --qemu-gic  phone RAM and framebuffer, but QEMU's GIC/timer: lets tools/qemu_loader.py run
           the real phone image (shim/loader) past interrupt setup.
  --root-extents JSON  a pinned file in userdata (tools/pinfile `extents --json`) as the
           extent root: /chosen fuxi-root-extents + fuxi-root-blocks (patch 0088), the whole
           file, unless --root-bytes N says how much of it.
  --log-extents JSON   a second pinned file for the kernel's console log (patch 0129):
           /chosen pandora-log-extents + pandora-log-blocks, same layout.
"""
import hashlib, json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Phone: the contiguous 5 GiB bank with no reservations (DT_FINDINGS.md; same bank as ios-fuxi).
PHYS_BASE = 0x8c0000000
MEM_SIZE = 0x140000000
# QEMU twin (virt RAM starts at 0x40000000)
QEMU_PHYS_BASE = 0xb0000000
QEMU_MEM_SIZE = 0x4d000000
# Panic log buffer, carved from the TOP of the dram range and exposed to XNU as
# /pram + /chosen/embedded-panic-log-size. Without it, pe_init.c's
# check_for_panic_log() leaves gPanicBase == 0, debug_log_init() bails out twice
# and the kernel hangs right after "kdp_core zlib memory" — reproduced on QEMU
# and independently on ios-fajita.
PANIC_LOG_SIZE = 0x100000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17000000, 0x10000
GICR, GICR_SIZE = 0x17080000, 0x200000   # 8 redistributors, stride 0x40000
FB, FB_SIZE = 0xfc800000, 0x2b00000      # splash_region; ARGB8888, measured by shim/hello
FB_W, FB_H, FB_STRIDE = 1220, 2656, 1220 * 4
QEMU_FB, QEMU_FB_SIZE = 0x9c000000, 0x1700000
QEMU_FB_W, QEMU_FB_H, QEMU_FB_STRIDE = 1080, 2400, 1080 * 4
PANDORA_ECID = 0x50414e444f524101  # 'PANDORA' + 1, synthetic
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 27                        # standard PPI 11 on SM8850
# MPIDRs of the eight Oryon cores (cpu@0..500 cluster 0, cpu@10000/10100 cluster 1)
MPIDRS = [0x0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x10000, 0x10100]
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.environ.get('D22AP_ADT', '/opt/work/ipsw/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def log_extent_props(path):
    """/chosen props for the console log file (patch 0129), same triples as the root."""
    props = root_extent_props(path)
    return [('pandora-log-extents', props[0][1]), ('pandora-log-blocks', props[1][1])]


def root_extent_props(path, root_bytes=None):
    """/chosen props for a pinned-file root (as ios-fuxi mkadt-fuxi.py): u64 triples
    {file block, partition block, blocks} in 4 KiB units, partition-relative. pinfile's
    JSON gives bytes: [logical, physical, length, fiemap flags]."""
    table = json.load(open(path))
    root_bytes = table['size'] if root_bytes is None else root_bytes
    if root_bytes <= 0 or root_bytes % 4096 or root_bytes > table['size']:
        raise ValueError('root size must be 4 KiB aligned and inside the pinned file')
    words, pos = [], 0
    for logical, physical, length, _flags in table['extents']:
        if logical != pos or physical % 4096 or length % 4096 or not length:
            raise ValueError('extent table is not contiguous 4 KiB extents')
        words += [logical // 4096, physical // 4096, length // 4096]
        pos += length
    if pos < root_bytes:
        raise ValueError('extents do not cover the root')
    return [('fuxi-root-extents', struct.pack(f'<{len(words)}Q', *words)),
            ('fuxi-root-blocks', struct.pack('<Q', root_bytes // 4096))]


def build(seed=None, qemu=False, qemu_gic=False, root_props=()):
    """qemu=True: QEMU virt twin (GIC, CNTFRQ, RAM, framebuffer). qemu_gic=True: only GIC/CNTFRQ."""
    phys_base, mem_size = (QEMU_PHYS_BASE, QEMU_MEM_SIZE) if qemu else (PHYS_BASE, MEM_SIZE)
    fb, fb_size, fb_w, fb_h, fb_stride = ((QEMU_FB, QEMU_FB_SIZE, QEMU_FB_W, QEMU_FB_H, QEMU_FB_STRIDE)
                                          if qemu else (FB, FB_SIZE, FB_W, FB_H, FB_STRIDE))
    # Carve the panic log off the top so the kernel never allocates over it.
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu or qemu_gic else (GICD, GICR, TIMEBASE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('pandora-shim')),
        ('system-firmware-version', cstr('pandora-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', PANDORA_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ] + list(root_props), [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # Eight Oryon cores in DT order with their real MPIDRs, described as one homogeneous
    # cluster like ios-sweet (no AMP code paths). cpus=N selects a prefix.
    # QEMU keeps a single CPU (MPIDR 0).
    cpu_nodes = []
    for core in range(1 if qemu or qemu_gic else len(MPIDRS)):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,sm8850-cpu') + cstr('ARM,v8')),
            ('reg', u32(MPIDRS[core])),  # actual MPIDR, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(4000000000 if core >= 6 else 3500000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu or qemu_gic else VTIMER_INTID)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(fb, fb_size)),
        ('width', u32(fb_w)),
        ('height', u32(fb_h)),
        ('stride', u32(fb_stride)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Panic log region. /pram reg = <phys size>; it sits just above the
    # dram-size we expose, so XNU maps it but never allocates into it.
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,pandora' is what SweetPlatformExpert matches (patch 0110).
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,pandora') + cstr('qcom,sm8850') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + ([] if qemu else [ufs]))


if __name__ == '__main__':
    def opt(name):
        return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None
    props = ()
    if opt('--root-extents'):
        rb = opt('--root-bytes')
        props = root_extent_props(opt('--root-extents'), int(rb, 0) if rb else None)
        print(f'root extents: {len(props[0][1]) // 24}, {struct.unpack("<Q", props[1][1])[0]} blocks',
              file=sys.stderr)
    if opt('--log-extents'):
        lp = log_extent_props(opt('--log-extents'))
        print(f'log extents: {len(lp[0][1]) // 24}, {struct.unpack("<Q", lp[1][1])[0]} blocks',
              file=sys.stderr)
        props = list(props) + lp
    root = build(qemu='--qemu' in sys.argv, qemu_gic='--qemu-gic' in sys.argv, root_props=props)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
