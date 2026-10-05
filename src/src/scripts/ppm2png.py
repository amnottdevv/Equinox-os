#!/usr/bin/env python3
"""ppm2png.py <in.ppm> <out.png> — konversi PPM (P6) ke PNG tanpa PIL."""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise RuntimeError("not a raw PPM")
    i, dims = 3, []
    while len(dims) < 3:
        while data[i:i + 1] in b" \t\r\n":
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in b"\r\n":
                i += 1
            continue
        j = i
        while data[j:j + 1].isdigit():
            j += 1
        dims.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = dims
    return w, h, data[i:i + w * h * 3]


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)                      # filter: None
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    w, h, px = read_ppm(sys.argv[1])
    write_png(sys.argv[2], w, h, px)
    print(f"{sys.argv[2]} {w}x{h}")
