#!/usr/bin/env python3
"""Generate the EpoCanva application icon from code.

The icon is an original bitmap drawn here (no third-party asset, no network access):
a blue rounded square with a white capital E. Sizes 16, 32 and 48 are packed into a
classic 32-bit ICO container so both the settings executable and the TSF DLL can carry
it as a resource. Run from the repository root:

    python tools/product/make_app_icon.py platforms/windows/product/EpoCanva.ico
"""
import struct
import sys


def draw(size: int) -> bytes:
    # BGRA pixels, top-down for now; the ICO DIB wants bottom-up.
    pixels = [[(0, 0, 0, 0)] * size for _ in range(size)]
    radius = max(1, size // 8)
    blue = (0xD0, 0x6D, 0x1C, 0xFF)  # BGRA of #1C6DD0
    white = (0xFF, 0xFF, 0xFF, 0xFF)
    for y in range(size):
        for x in range(size):
            # Rounded square mask.
            dx = 0
            if x < radius:
                dx = radius - x
            elif x >= size - radius:
                dx = x - (size - radius - 1)
            dy = 0
            if y < radius:
                dy = radius - y
            elif y >= size - radius:
                dy = y - (size - radius - 1)
            if dx * dx + dy * dy <= radius * radius:
                pixels[y][x] = blue

    def fill(left: float, top: float, right: float, bottom: float) -> None:
        x0 = max(0, int(round(left * size)))
        x1 = min(size, int(round(right * size)))
        y0 = max(0, int(round(top * size)))
        y1 = min(size, int(round(bottom * size)))
        for y in range(y0, y1):
            for x in range(x0, x1):
                pixels[y][x] = white

    # The E: stem plus three bars.
    fill(0.28, 0.22, 0.43, 0.78)
    fill(0.28, 0.22, 0.74, 0.33)
    fill(0.28, 0.45, 0.68, 0.56)
    fill(0.28, 0.68, 0.74, 0.78)

    header = struct.pack(
        "<IiiHHIIiiII",
        40,          # biSize
        size,        # biWidth
        size * 2,    # biHeight (XOR + AND masks)
        1,           # biPlanes
        32,          # biBitCount
        0,           # biCompression
        size * size * 4,
        0, 0, 0, 0,
    )
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            b, g, r, a = pixels[y][x]
            xor += bytes((b, g, r, a))
    mask_row = ((size + 31) // 32) * 4
    and_mask = bytes(mask_row * size)
    return header + bytes(xor) + and_mask


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: make_app_icon.py <output.ico>", file=sys.stderr)
        return 2
    sizes = (16, 32, 48)
    images = [draw(size) for size in sizes]
    directory = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = bytearray()
    for size, image in zip(sizes, images):
        entries += struct.pack(
            "<BBBBHHII",
            size if size < 256 else 0,
            size if size < 256 else 0,
            0, 0, 1, 32, len(image), offset,
        )
        offset += len(image)
    with open(sys.argv[1], "wb") as stream:
        stream.write(directory)
        stream.write(entries)
        for image in images:
            stream.write(image)
    print(f"wrote {sys.argv[1]}: {len(images)} images, {offset} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
