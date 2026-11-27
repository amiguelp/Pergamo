#!/usr/bin/env python3
"""Prepara los archivos que usa la actualización automática de Pergamo.

Lee PERGAMO_VERSION de opds_app.h y escribe, junto a Pergamo.app:
  version.txt      -> la versión (p. ej. "1.2")
  Pergamo.app.b2   -> BLAKE2b-256 en hexadecimal de Pergamo.app

Los tres archivos deben subirse a main en el mismo commit. Los lectores
comprueban version.txt, descargan Pergamo.app y verifican la huella.
Uso: python3 tools/make_release_files.py   (o "make release")
"""
import hashlib
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    header = open(os.path.join(ROOT, "opds_app.h"), encoding="utf-8").read()
    match = re.search(r'#define\s+PERGAMO_VERSION\s+"([0-9.]+)"', header)
    if not match:
        sys.exit("No se encontró PERGAMO_VERSION en opds_app.h")
    version = match.group(1)

    app_path = os.path.join(ROOT, "Pergamo.app")
    data = open(app_path, "rb").read()
    if data[:4] != b"\x7fELF" or data[18] != 40:
        sys.exit("Pergamo.app no es un ejecutable ARM")
    if version.encode() not in data:
        sys.exit(f"Pergamo.app no parece compilada con la versión {version}; ejecuta make")

    digest = hashlib.blake2b(data, digest_size=32).hexdigest()
    with open(os.path.join(ROOT, "version.txt"), "w", newline="\n") as f:
        f.write(version + "\n")
    with open(os.path.join(ROOT, "Pergamo.app.b2"), "w", newline="\n") as f:
        f.write(digest + "  Pergamo.app\n")
    print(f"version.txt = {version}")
    print(f"Pergamo.app.b2 = {digest}")


if __name__ == "__main__":
    main()
