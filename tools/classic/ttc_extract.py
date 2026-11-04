#!/usr/bin/env python3
"""Extract one face of an OpenType collection (.ttc) into a standalone .otf/.ttf.

Usage: ttc_extract.py <in.ttc> <index> <out.otf>
Dependency-free (no fontTools): copies the face's table directory and tables,
re-laying them out 4-byte aligned. Table checksums are recomputed; head's
checkSumAdjustment is left as-is (renderers do not verify it).
"""
import struct
import sys


def checksum(data):
    data = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(data) // 4}I", data)) & 0xFFFFFFFF


def extract(src, index):
    if src[:4] != b"ttcf":
        raise SystemExit("not a TTC")
    num = struct.unpack(">I", src[8:12])[0]
    if not 0 <= index < num:
        raise SystemExit(f"index {index} out of range (0..{num - 1})")
    off = struct.unpack(">I", src[12 + 4 * index:16 + 4 * index])[0]
    sfnt, ntables = struct.unpack(">IH", src[off:off + 6])
    entries = []
    for i in range(ntables):
        tag, _, toff, tlen = struct.unpack(">4sIII", src[off + 12 + 16 * i:off + 28 + 16 * i])
        entries.append((tag, src[toff:toff + tlen]))
    entries.sort(key=lambda e: e[0])
    es = max(k for k in range(16) if (1 << k) <= ntables)
    search = (1 << es) * 16
    header = struct.pack(">IHHHH", sfnt, ntables, search, es, ntables * 16 - search)
    pos = 12 + 16 * ntables
    dirs, blobs = [], []
    for tag, data in entries:
        dirs.append(struct.pack(">4sIII", tag, checksum(data), pos, len(data)))
        pad = data + b"\0" * (-len(data) % 4)
        blobs.append(pad)
        pos += len(pad)
    return header + b"".join(dirs) + b"".join(blobs)


if __name__ == "__main__":
    with open(sys.argv[1], "rb") as f:
        out = extract(f.read(), int(sys.argv[2]))
    with open(sys.argv[3], "wb") as f:
        f.write(out)
