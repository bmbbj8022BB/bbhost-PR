#!/usr/bin/env python3
"""Adds the Xbox button pictures as KG_<name>_XB textures to a Bloodborne PS4 menu/common.tpf.dcx.

  python tools/make_xb_tpf.py --base <the game's menu/common.tpf.dcx> \
      --pack <Modern Xbox prompts menu/common.tpf.dcx> --out <MODS/menu/common.tpf.dcx>

The game's own textures are kept as they are; only 20 new ones are appended
(the pack's KG_* pictures under a _XB name). PS4 layout, DCX DFLT, UTF-16 names.
"""
import argparse, struct, sys, zlib

NAMES = ["Cancel", "L1", "L2", "L3", "LS", "L_D", "L_LR", "L_U", "L_UD", "L_UDLR", "OK",
         "R1", "R2", "R3", "RS", "R_L", "R_U", "Start", "TP_L", "TP_R"]
STRIDE = 36

def die(msg):
    sys.exit("make_xb_tpf: " + msg)

def read_dcx(path):
    d = open(path, "rb").read()
    if d[:4] != b"DCX\0" or d[0x28:0x2c] != b"DFLT" or d[0x44:0x48] != b"DCA\0" or d[0x4c:0x4e] != b"\x78\xda":
        die(path + ": not a DCX DFLT file of the expected layout")
    return d[:0x4c], zlib.decompress(d[0x4c:])

def parse_tpf(raw, path):
    if raw[:4] != b"TPF\0":
        die(path + ": not a TPF")
    ds, fc = struct.unpack("<ii", raw[4:12])
    plat, flag2, enc, pad = raw[12], raw[13], raw[14], raw[15]
    if plat != 4 or enc != 1:
        die("%s: platform %d encoding %d, expected PS4 (4) and UTF-16 (1)" % (path, plat, enc))
    ents = []
    for i in range(fc):
        h = raw[16 + i * STRIDE: 16 + (i + 1) * STRIDE]
        off, size = struct.unpack("<ii", h[:8])
        nameoff, hasfloat = struct.unpack("<ii", h[24:32])
        if hasfloat != 0:
            die("%s: entry %d has a float struct, not handled" % (path, i))
        e = nameoff
        while raw[e:e + 2] != b"\0\0" or (e - nameoff) % 2:
            e += 1
        name = raw[nameoff:e].decode("utf-16le")
        ents.append({"hdr": bytearray(h), "name": name, "data": raw[off:off + size]})
    if struct.unpack("<i", ents[0]["hdr"][24:28])[0] != 16 + fc * STRIDE:
        die(path + ": names do not follow the entry table")
    return (plat, flag2, enc, pad), ents

def build(meta, ents):
    plat, flag2, enc, pad = meta
    n = len(ents)
    pos = 16 + n * STRIDE
    names = b""
    for e in ents:
        e["nameoff"] = pos + len(names)
        names += e["name"].encode("utf-16le") + b"\0\0"
    data_start = pos + len(names)
    data = b""
    for e in ents:
        e["off"] = data_start + len(data)
        data += e["data"]
    table = b""
    for e in ents:
        h = bytearray(e["hdr"])
        h[0:4] = struct.pack("<i", e["off"])
        h[24:28] = struct.pack("<i", e["nameoff"])
        table += bytes(h)
    return b"TPF\0" + struct.pack("<ii", len(data), n) + bytes([plat, flag2, enc, pad]) + table + names + data

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    dcx_hdr, base_raw = read_dcx(a.base)
    _, pack_raw = read_dcx(a.pack)
    meta, base = parse_tpf(base_raw, a.base)
    _, pack = parse_tpf(pack_raw, a.pack)
    have = {e["name"] for e in base}
    by = {e["name"]: e for e in pack}
    for n in NAMES:
        new = "KG_%s_XB" % n
        if new in have:
            die(new + " is already in the base file")
        if "KG_" + n not in by:
            die("KG_%s is not in the pack" % n)
        if "KG_" + n not in have:
            print("note: the base has no KG_%s" % n)
        src = by["KG_" + n]
        base.append({"hdr": bytearray(src["hdr"]), "name": new, "data": src["data"]})
    raw = build(meta, base)
    comp = zlib.compress(raw, 9)
    hdr = bytearray(dcx_hdr)
    hdr[0x1c:0x20] = struct.pack(">I", len(raw))
    hdr[0x20:0x24] = struct.pack(">I", len(comp))
    open(a.out, "wb").write(bytes(hdr) + comp)
    print("wrote %s: %d textures (%d new), %d bytes" % (a.out, len(base), len(NAMES), len(hdr) + len(comp)))

main()
