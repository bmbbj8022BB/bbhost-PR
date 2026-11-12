#!/usr/bin/env python3
"""Writes the Xbox button pictures out of a PS4 menu/common.tpf.dcx as a C++ include (src/engine/xb_glyphs.inc).

  python tools/xb_pack_to_inc.py <Modern Xbox prompts menu/common.tpf.dcx> src/engine/xb_glyphs.inc

Each picture is its 36-byte TPF entry header and its texture data, as make_xb_tpf.py copies them.
"""
import struct, sys, zlib

NAMES = ["Cancel", "L1", "L2", "L3", "LS", "L_D", "L_LR", "L_U", "L_UD", "L_UDLR", "OK",
         "R1", "R2", "R3", "RS", "R_L", "R_U", "Start", "TP_L", "TP_R"]
STRIDE = 36

def die(m):
    sys.exit("xb_pack_to_inc: " + m)

def main():
    if len(sys.argv) != 3:
        die("usage: xb_pack_to_inc.py <pack common.tpf.dcx> <out .inc>")
    d = open(sys.argv[1], "rb").read()
    if d[:4] != b"DCX\0" or d[0x28:0x2c] != b"DFLT" or d[0x44:0x48] != b"DCA\0" or d[0x4c:0x4e] != b"\x78\xda":
        die("not a DCX DFLT file of the expected layout")
    raw = zlib.decompress(d[0x4c:])
    if raw[:4] != b"TPF\0":
        die("not a TPF")
    fc = struct.unpack("<i", raw[8:12])[0]
    if raw[12] != 4 or raw[14] != 1:
        die("expected PS4 (4) and UTF-16 (1)")
    ents = {}
    for i in range(fc):
        h = raw[16 + i * STRIDE: 16 + (i + 1) * STRIDE]
        off, size = struct.unpack("<ii", h[:8])
        no = struct.unpack("<i", h[24:28])[0]
        if struct.unpack("<i", h[28:32])[0] != 0:
            die("entry %d has a float struct" % i)
        e = no
        while raw[e:e + 2] != b"\0\0" or (e - no) % 2:
            e += 1
        ents[raw[no:e].decode("utf-16le")] = (h, raw[off:off + size])
    def arr(b):
        return ",".join(str(x) for x in b)
    out = ["// Made by tools/xb_pack_to_inc.py: the Xbox button pictures (PS4 TPF entry header + texture data).\n"]
    for n in NAMES:
        if "KG_" + n not in ents:
            die("KG_%s is not in the pack" % n)
        h, dat = ents["KG_" + n]
        out.append("static const unsigned char kXbHdr_%s[%d] = {%s};\n" % (n, STRIDE, arr(h)))
        out.append("static const unsigned char kXbDat_%s[%d] = {%s};\n" % (n, len(dat), arr(dat)))
    out.append("static const XbTexture kXbTextures[] = {\n")
    for n in NAMES:
        out.append('    {"KG_%s_XB", kXbHdr_%s, kXbDat_%s, sizeof(kXbDat_%s)},\n' % (n, n, n, n))
    out.append("};\n")
    open(sys.argv[2], "w").write("".join(out))
    print("wrote %s: %d pictures, %d bytes of texture data" % (sys.argv[2], len(NAMES), sum(len(ents["KG_" + n][1]) for n in NAMES)))

main()
