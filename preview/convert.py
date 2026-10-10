"""Convert the headless renderer's original PPM pixel buffers to PNG, losslessly."""
from pathlib import Path
import struct
import sys
import zlib


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def ppm_to_png(path: Path) -> None:
    with path.open("rb") as source:
        if source.readline() != b"P6\n":
            raise ValueError(f"Unexpected PPM format: {path}")
        width, height = map(int, source.readline().split())
        if source.readline() != b"255\n":
            raise ValueError(f"Unexpected PPM color depth: {path}")
        rgb = source.read()
    if len(rgb) != width * height * 3:
        raise ValueError(f"Wrong pixel buffer size: {path}")
    stride = width * 3
    scanlines = b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    data = b"\x89PNG\r\n\x1a\n"
    data += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    data += png_chunk(b"IDAT", zlib.compress(scanlines, 9))
    data += png_chunk(b"IEND", b"")
    path.with_suffix(".png").write_bytes(data)

folder = Path(sys.argv[1] if len(sys.argv) > 1 else "screens")
for path in sorted(folder.glob("*.ppm")):
    ppm_to_png(path)
    print(path.with_suffix(".png"))

