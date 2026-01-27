# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pillow",
#     "numpy",
# ]
# ///

from PIL import Image
import numpy as np
import struct
import sys
import os

TARGET_W = 480
TARGET_H = 670

def convert_image(in_path, out_path):
    img = Image.open(in_path).convert("RGB")

    # Forzar tamaño exacto
    if img.size != (TARGET_W, TARGET_H):
        print(f"[INFO] Redimensionando {img.size} -> {(TARGET_W, TARGET_H)}")
        img = img.resize((TARGET_W, TARGET_H), resample=Image.NEAREST)

    arr = np.array(img)

    r = arr[:, :, 0]
    g = arr[:, :, 1]
    b = arr[:, :, 2]

    # Clasificación robusta a blanco / negro / rojo
    red_mask = (r > 160) & (g < 120) & (b < 120)
    lum = (0.2126*r + 0.7152*g + 0.0722*b)
    black_mask = (lum < 128) & (~red_mask)
    white_mask = ~(red_mask | black_mask)

    # Empaquetado bit a bit (MSB first)
    w, h = TARGET_W, TARGET_H
    bytes_per_row = (w + 7) // 8

    black_plane = bytearray(bytes_per_row * h)
    red_plane   = bytearray(bytes_per_row * h)

    for y in range(h):
        row_off = y * bytes_per_row
        for x in range(w):
            bit = 7 - (x % 8)
            idx = row_off + (x // 8)

            if black_mask[y, x]:
                black_plane[idx] |= (1 << bit)
            elif red_mask[y, x]:
                red_plane[idx] |= (1 << bit)
            # blanco => no se marca nada

    with open(out_path, "wb") as f:
        f.write(b"TRI1")
        f.write(struct.pack("<H", w))
        f.write(struct.pack("<H", h))
        f.write(black_plane)
        f.write(red_plane)

    print(f"[OK] {in_path} -> {out_path}")
    print(f"     Tamaño: {len(black_plane)+len(red_plane)+8} bytes")

def main():
    if len(sys.argv) < 2:
        print("Uso: python png2tri.py imagen1.png [imagen2.png ...]")
        return

    for in_file in sys.argv[1:]:
        base = os.path.splitext(in_file)[0]
        out_file = base + ".tri"
        convert_image(in_file, out_file)

if __name__ == "__main__":
    main()
