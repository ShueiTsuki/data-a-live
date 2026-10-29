"""spk_graph.py ACDB OUT: the speaker playback graph of Android's ACDB as the audio DSP takes it.

Reads the phone's ACDB (/odm/etc/acdbdata/Mi/Mi_acdb_cal.acdb; a copy of a phone file, kept outside
the repo, as OUT is) and writes what bsd/dev/pandora_adsp.c sends to the DSP's APM to open and calibrate
the graph Android opens for a speaker playback (D:\\DeepSeekHarness\\spkstart, acdbcheck): graph key
vector StreamRX PCM_Deep_Buffer, Instance 1, DeviceRX Speaker, DevicePP_Rx Audio_MIPLAY; calibration
key vector Volume 0.

Volume picks one of the stream subgraph's 16 tables for its volume module 0x401f (acdbcheck
raw/ckv_volume_table.txt): Level_0 has the gain 0x10000000 (Q28, 0 dB), each level about 3 dB less, Level_15
0x00170894 (-45 dB). The Android playback that was traced had Level_15 at media volume 0; iOS scales the
samples by its own volume before they reach the ring, so the DSP takes them at Level_0, Android's full volume.

The layouts are AudioReach graphservices' (acdb/src/acdb_command.c, gsl/src/gsl_graph.c):
  OPEN  the APM_CMD_GRAPH_OPEN payload: AcdbCmdGetSubgraphData's SPF blob (each subgraph's SPF
        properties as <MIID, PID, size, error code 0, data padded to 8>, BuildSpfPropertyBlob), then
        AcdbCmdGetSubgraphConnections' blob (each <src, dst> subgraph edge's SCLU/SCDE/SCDO parameters,
        same layout). gsl_graph_open sends exactly these two back to back.
  CAL   the APM_CMD_SET_CFG payload of the non-persistent calibration (AcdbCmdGetSubgraphCalDataNonPersist
        on a new graph): per subgraph, the default table and every table whose keys the CKV matches; a
        module with data in a matched table drops its default-table data (AcdbGetSubgraphCalibration's
        default IID list).
  TDM   the device_hw_ep_rx tag's data for Channels 2 (AcdbCmdGetModuleTagData; AGM configure_tdm_ep),
        PARAM_ID_TDM_INTF_CFG among it; pandora_adsp.c fills in LPAIF type and interface index.
  SGS   the subgraph ids, for PREPARE, START, STOP and CLOSE.

OUT: "PSPKGR01", u32 section count, then sections {u32 type (1 OPEN, 2 CAL, 3 TDM, 4 SGS), u32 bytes,
data padded to 8}.
"""
import struct
import sys

GKV = {0xa1000000: 0xa1000001, 0xab000000: 0x1, 0xa2000000: 0xa2000001, 0xac000000: 0xac000100}
CKV = {0xa4000000: 0x0}                # Volume Level_0
TAG_HW_EP_RX = 0xc0000004
TKV = {0xa9000000: 2}                  # Channels
PARAM_ID_TDM_INTF_CFG = 0x0800101B


def tag(name):
    return struct.unpack('<I', name.encode())[0]


def pad8(b):
    return b + bytes(-len(b) % 8)


class Acdb:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        fid, _, _ = struct.unpack_from('<III', self.d, 0)
        assert fid == 0x42444341, 'not an ACDB file'
        self.chunks = {}
        o = 12
        while o + 8 <= len(self.d):
            cid, sz = struct.unpack_from('<II', self.d, o)
            self.chunks.setdefault(cid, (o + 8, sz))
            o += 8 + sz
        assert o == len(self.d), 'chunk chain does not end at the end of the file'

    def u32(self, o, n=1):
        v = struct.unpack_from('<%dI' % n, self.d, o)
        return v[0] if n == 1 else list(v)

    def chunk(self, name):
        return self.chunks[tag(name)]

    def pool_param(self, pool_off):
        o = self.chunk('POOL')[0] + pool_off
        size = self.u32(o)
        return self.d[o + 4:o + 4 + size]

    # GKVT / GKVL: the graph's subgraph list and properties
    def find_graph(self, gkv):
        base, _ = self.chunk('GKVT')
        keys = sorted(gkv)
        o = base
        ntab = self.u32(o)
        o += 4
        for _ in range(ntab):
            nk, ne = self.u32(o, 2)
            o += 8
            for _ in range(ne):
                kids = self.u32(o, nk) if nk else []
                if nk == 1:
                    kids = [kids]
                lut = self.u32(o + 4 * nk)
                o += 4 * nk + 4
                if nk == len(keys) and list(kids) == keys:
                    lb = self.chunk('GKVL')[0] + lut
                    lnk, lne = self.u32(lb, 2)
                    p = lb + 8
                    for _ in range(lne):
                        vals = self.u32(p, lnk) if lnk > 1 else [self.u32(p)]
                        sg_list, sg_prop = self.u32(p + 4 * lnk, 2)
                        p += 4 * lnk + 8
                        if vals == [gkv[k] for k in keys]:
                            return sg_list, sg_prop
        raise SystemExit('graph key vector not in the ACDB')

    def sg_list(self, off):
        o = self.chunk('POOL')[0] + off
        size, n = self.u32(o, 2)
        o += 8
        out = []
        for _ in range(n):
            sg, ndst = self.u32(o, 2)
            o += 8
            dst = [self.u32(o + 4 * i) for i in range(ndst)]
            o += 4 * ndst
            out.append((sg, dst))
        return out

    def sg_spf_blob(self, sg_prop_off, sg_id):
        """BuildSubgraphDataRsp + BuildSpfPropertyBlob for one subgraph."""
        pool, psz = self.chunk('POOL')
        o = pool + sg_prop_off + 8
        while o < pool + psz:
            sid, dsize = self.u32(o, 2)
            body = o + 8
            if sid == sg_id:
                drv = self.u32(body)
                spf = body + 4 + drv
                ssz = self.u32(spf)
                p, end, out = spf + 4, spf + 4 + ssz, b''
                while p < end:
                    miid, pid, size = self.u32(p, 3)
                    data = self.d[p + 12:p + 12 + size]
                    out += struct.pack('<IIII', miid, pid, size, 0) + pad8(data)
                    p += 12 + size + (-size % 8)
                assert p == end, 'SPF properties of %#x do not end at their size' % sg_id
                return out
            o = body + dsize
        raise SystemExit('subgraph %#x has no properties' % sg_id)

    def sg_connections(self, edges):
        """AcdbCmdGetSubgraphConnections: SCLU {u32 count?, entries <src, dst, def, dot>}."""
        lut, lsz = self.chunk('SCLU')
        entries = {}
        for o in range(lut + 4, lut + lsz, 16):
            src, dst, def_off, dot_off = self.u32(o, 4)
            entries[(src, dst)] = (def_off, dot_off)
        out = b''
        for src, dst in edges:
            if (src, dst) not in entries:
                print('  no connection data for %#x -> %#x' % (src, dst))
                continue
            def_off, dot_off = entries[(src, dst)]
            out += self.def_dot('SCDE', 'SCDO', def_off, dot_off)
        return out

    def def_dot(self, dname, oname, def_off, dot_off, skip=()):
        """{n, n x <miid, pid>} with {n, n x pool offset} as <MIID, PID, size, 0, data padded to 8>."""
        d = self.chunk(dname)[0] + def_off
        t = self.chunk(oname)[0] + dot_off
        n, nt = self.u32(d), self.u32(t)
        assert n == nt, 'def and dot tables differ'
        out, params = b'', []
        for i in range(n):
            miid, pid = self.u32(d + 4 + 8 * i, 2)
            if miid in skip:
                continue
            data = self.pool_param(self.u32(t + 4 + 4 * i))
            out += struct.pack('<IIII', miid, pid, len(data), 0) + pad8(data)
            params.append((miid, pid, len(data)))
        self.last_params = params
        return out

    def calibration(self, sg_ids, ckv):
        """Non-persistent calibration of the subgraphs for a CKV, as on a new graph."""
        cslu, _ = self.chunk('CSLU')
        nsg = self.u32(cslu)
        o, tables = cslu + 4, {}
        for _ in range(nsg):
            sg, ne = self.u32(o, 2)
            o += 8
            tables[sg] = [tuple(self.u32(o + 8 * i, 2)) for i in range(ne)]
            o += 8 * ne
        out, count = b'', 0
        for sg in sg_ids:
            default, matched = None, []
            for kt, lut in tables.get(sg, []):
                kb = self.chunk('CAKT')[0] + kt
                nk = self.u32(kb)
                keys = [self.u32(kb + 4 + 4 * i) for i in range(nk)]
                lb = self.chunk('CDLU')[0] + lut
                lnk, lne = self.u32(lb, 2)
                assert lnk == nk
                if any(k not in ckv for k in keys):
                    continue
                want = [ckv[k] for k in keys]
                p = lb + 8
                for _ in range(lne):
                    vals = [self.u32(p + 4 * i) for i in range(lnk)]
                    def_off, dot_off, _ = self.u32(p + 4 * lnk, 3)
                    p += 4 * lnk + 12
                    if vals == want:
                        if nk == 0:
                            default = (def_off, dot_off)
                        else:
                            matched.append((def_off, dot_off))
                        break
            override = set()
            for def_off, _ in matched:
                d = self.chunk('CDDE')[0] + def_off
                override |= {self.u32(d + 4 + 8 * i) for i in range(self.u32(d))}
            parts = ([(default, override)] if default else []) + [(m, ()) for m in matched]
            for (def_off, dot_off), skip in parts:
                out += self.def_dot('CDDE', 'CDDO', def_off, dot_off, skip)
                count += len(self.last_params)
            print('  calibration of %#x: default %s, %d matched table(s), %d module(s) overridden' % (
                sg, 'yes' if default else 'no', len(matched), len(override)))
        return out, count

    def tag_data(self, sg_ids, tag_id, tkv):
        """AcdbCmdGetModuleTagData: MTKT <sg, tag, offset>, MTLU {nk, ne, <values, def, dot>}."""
        kt, ksz = self.chunk('MTKT')
        found = {}
        for o in range(kt + 4, kt + ksz, 12):
            sg, t, off = self.u32(o, 3)
            found[(sg, t)] = off
        want = [tkv[k] for k in sorted(tkv)]
        out = b''
        for sg in sg_ids:
            if (sg, tag_id) not in found:
                continue
            lb = self.chunk('MTLU')[0] + found[(sg, tag_id)]
            nk, ne = self.u32(lb, 2)
            p = lb + 8
            for _ in range(ne):
                vals = [self.u32(p + 4 * i) for i in range(nk)]
                def_off, dot_off = self.u32(p + 4 * nk, 2)
                p += 4 * nk + 8
                if nk == len(want) and vals == want:
                    out += self.def_dot('MTDE', 'MTDO', def_off, dot_off)
                    print('  tag %#x data in %#x: %s' % (tag_id, sg, ', '.join(
                        'miid %#x pid %#x %d B' % x for x in self.last_params)))
        return out


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    a = Acdb(argv[1])
    sg_list_off, sg_prop_off = a.find_graph(GKV)
    sgs = a.sg_list(sg_list_off)
    print('subgraphs (sink first): %s' % ', '.join('%#x -> %s' % (s, [hex(x) for x in d]) for s, d in sgs))
    sg_ids = [s for s, _ in sgs]
    spf = b''.join(a.sg_spf_blob(sg_prop_off, s) for s in sg_ids)
    conn = a.sg_connections([(s, d) for s, ds in sgs for d in ds])
    cal, ncal = a.calibration(sg_ids, CKV)
    tdm = a.tag_data(sg_ids, TAG_HW_EP_RX, TKV)
    o = 0
    while o + 16 <= len(tdm):
        miid, pid, size, _ = struct.unpack_from('<IIII', tdm, o)
        if pid == PARAM_ID_TDM_INTF_CFG and size >= 32:
            f = struct.unpack_from('<IIHHIHHHHHHHH', tdm, o + 16)
            print('  TDM interface config: lpaif %d intf %d sync_src %d data_oe %d slot_mask %#x slots %d width %d '
                  'sync_mode %d invert_sync %d data_delay %d lane_mask %#x' % (
                      f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[11]))
        o += 16 + size + (-size % 8)
    sections = [(1, spf + conn), (2, cal), (3, tdm), (4, struct.pack('<I%dI' % len(sg_ids), len(sg_ids), *sg_ids))]
    blob = b'PSPKGR01' + struct.pack('<I', len(sections))
    for t, data in sections:
        blob += struct.pack('<II', t, len(data)) + pad8(data)
    open(argv[2], 'wb').write(blob)
    print('open %d B (subgraphs %d + connections %d), calibration %d B in %d parameters, TDM %d B; %s %d B' % (
        len(spf) + len(conn), len(spf), len(conn), len(cal), ncal, len(tdm), argv[2], len(blob)))


if __name__ == '__main__':
    main(sys.argv)
