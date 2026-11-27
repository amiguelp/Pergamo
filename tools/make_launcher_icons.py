#!/usr/bin/env python3
"""Genera los iconos del launcher de PocketBook (Pergamo.bmp y Pergamo_f.bmp).

Dibuja el icono como line-art vectorial al estilo de los iconos del sistema
(en el Verse Pro: 106 x 106 px con un dibujo de ~66 px y trazo de ~3 px,
medido sobre fotos del launcher; la wiki de MobileRead indica 122 x 122
para otros modelos) y lo
guarda como BMP de 24 bits, el formato que ya acepta el launcher.

Uso:  python3 tools/make_launcher_icons.py [carpeta_salida]
Requiere Pillow (pip install pillow).
"""
import os
import struct
import sys

from PIL import Image, ImageDraw

SIZE = 106          # tamaño de los iconos de aplicación del sistema (Verse Pro)
SS = 8              # supermuestreo para suavizar bordes
STROKE = 3.2        # grosor de línea en píxeles finales
GLYPH = 66          # lado del dibujo (margen como los del sistema)
DESIGN = 86         # el dibujo está diseñado en una cuadrícula de 86 unidades
K = GLYPH / DESIGN


def S(v):
    return int(round(v * SS))


def rounded_outline(d, box, radius, width, fg, bg=None):
    x0, y0, x1, y1 = box
    d.rounded_rectangle([S(x0), S(y0), S(x1), S(y1)], radius=S(radius),
                        fill=bg, outline=fg, width=S(width))


def draw_glyph(fg, bg):
    """Dibuja el glifo en un lienzo SIZE*SS con color de línea fg y fondo bg."""
    img = Image.new("L", (S(SIZE), S(SIZE)), bg)
    d = ImageDraw.Draw(img)
    ox = (SIZE - GLYPH) / 2
    oy = (SIZE - GLYPH) / 2

    # Coordenadas del diseño (0..86) -> píxeles. El grosor de línea NO se
    # escala: se mantiene en ~3 px como en los iconos del sistema.
    def X(v): return ox + v * K
    def Y(v): return oy + v * K

    # Pantalla / estantería
    rounded_outline(d, (X(0), Y(2), X(72), Y(60)), 7 * K, STROKE, fg)
    # Balda
    d.line([S(X(5)), S(Y(49)), S(X(67)), S(Y(49))], fill=fg, width=S(STROKE))
    # Libros de pie
    for bx, top in ((9, 15), (23, 11), (37, 17)):
        rounded_outline(d, (X(bx), Y(top), X(bx + 12), Y(49)), 2.5 * K, STROKE * 0.8, fg)
    # Libro inclinado
    book = Image.new("L", (S(13 * K), S(31 * K)), 0)
    bd = ImageDraw.Draw(book)
    bd.rounded_rectangle([S(0.6), S(0.6), S(13 * K - 0.6), S(31 * K - 0.6)], radius=S(2.5 * K),
                         outline=255, width=S(STROKE * 0.8))
    book = book.rotate(-20, resample=Image.BICUBIC, expand=True)
    ink = Image.new("L", book.size, fg)
    img.paste(ink, (S(X(50)), S(Y(16))), book)

    # Nube (tapa la esquina inferior derecha): contorno = forma grande - forma pequeña
    cloud_parts = [  # (cx, cy, r) en coordenadas del glifo
        (47, 66, 13), (63, 58, 16), (77, 68, 10),
    ]
    base = (35, 66, 87, 84)   # rectángulo inferior redondeado

    def cloud(draw, grow, color):
        for cx, cy, r in cloud_parts:
            rr = r * K + grow
            draw.ellipse([S(X(cx) - rr), S(Y(cy) - rr), S(X(cx) + rr), S(Y(cy) + rr)], fill=color)
        x0, y0, x1, y1 = base
        draw.rounded_rectangle([S(X(x0) - grow), S(Y(y0) - grow), S(X(x1) + grow), S(Y(y1) + grow)],
                               radius=S(8 * K + grow), fill=color)

    cloud(d, STROKE / 2, fg)     # borde exterior
    cloud(d, -STROKE / 2, bg)    # relleno (deja el contorno)

    # Libro abierto dentro de la nube
    w = STROKE * 0.75
    cx, top, bottom = 61, 64, 79
    d.line([S(X(cx)), S(Y(top + 2)), S(X(cx)), S(Y(bottom))], fill=fg, width=S(w))
    d.line([S(X(cx)), S(Y(top + 2)), S(X(cx - 11)), S(Y(top))], fill=fg, width=S(w), joint="curve")
    d.line([S(X(cx - 11)), S(Y(top)), S(X(cx - 11)), S(Y(bottom - 2))], fill=fg, width=S(w))
    d.line([S(X(cx - 11)), S(Y(bottom - 2)), S(X(cx)), S(Y(bottom))], fill=fg, width=S(w))
    d.line([S(X(cx)), S(Y(top + 2)), S(X(cx + 11)), S(Y(top))], fill=fg, width=S(w))
    d.line([S(X(cx + 11)), S(Y(top)), S(X(cx + 11)), S(Y(bottom - 2))], fill=fg, width=S(w))
    d.line([S(X(cx + 11)), S(Y(bottom - 2)), S(X(cx)), S(Y(bottom))], fill=fg, width=S(w))
    return img


def make_normal():
    big = draw_glyph(fg=0, bg=255)
    return big.resize((SIZE, SIZE), Image.LANCZOS)


def make_focused():
    """Variante seleccionada: círculo negro con el dibujo en blanco."""
    big = Image.new("L", (S(SIZE), S(SIZE)), 255)
    d = ImageDraw.Draw(big)
    d.ellipse([S(1), S(1), S(SIZE - 1), S(SIZE - 1)], fill=0)
    glyph = draw_glyph(fg=255, bg=0)
    circle = Image.new("L", big.size, 0)
    ImageDraw.Draw(circle).ellipse([S(1), S(1), S(SIZE - 1), S(SIZE - 1)], fill=255)
    big.paste(glyph, (0, 0), circle)
    return big.resize((SIZE, SIZE), Image.LANCZOS)


def write_bmp24(path, img):
    """BMP de 24 bits sin compresión (cabecera BITMAPINFOHEADER)."""
    img = img.convert("L")
    w, h = img.size
    stride = (w * 3 + 3) & ~3
    data = bytearray()
    px = img.load()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            v = px[x, y]
            row += bytes((v, v, v))
        row += b"\0" * (stride - w * 3)
        data += row
    header = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(data), 0, 0, 14 + 40)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(header + dib + data)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..")
    write_bmp24(os.path.join(out, "Pergamo.bmp"), make_normal())
    write_bmp24(os.path.join(out, "Pergamo_f.bmp"), make_focused())
    print("Iconos generados en", os.path.abspath(out))


if __name__ == "__main__":
    main()
