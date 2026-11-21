"""Reproduce the GPU notification change for the verified Android 16 image."""

import hashlib
import io
from pathlib import Path
import struct
import sys
import zipfile
import zlib


STOCK = "8c5b2d79045de041cde92f83d9949c15a4e7553216aa974f36e273c2da0711d2"
PATCHED = "f1f8131ffa032e15eb8195ba3d5205d8f210b2e3f1de068aaa6c571f1faa465c"


def main(source, target):
    original = Path(source).read_bytes()
    if hashlib.sha256(original).hexdigest() != STOCK:
        raise SystemExit("Expected the stock services.jar from the verified image.")

    result = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(original)) as src, zipfile.ZipFile(result, "w") as dst:
        for entry in src.infolist():
            data = src.read(entry)
            if entry.filename == "classes3.dex":
                if data.count(b"pastel\0") != 1:
                    raise SystemExit("Unexpected DEX string table.")
                dex = bytearray(data.replace(b"pastel\0", b"paste1\0"))
                dex[12:32] = hashlib.sha1(dex[32:]).digest()
                struct.pack_into("<I", dex, 8, zlib.adler32(dex[12:]))
                data = dex
            dst.writestr(entry, data)

    data = result.getvalue()
    if hashlib.sha256(data).hexdigest() != PATCHED:
        raise SystemExit("Result differs from the verified services.jar.")
    with Path(target).open("xb") as output:
        output.write(data)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python3 make_services_jar.py STOCK.jar NEW.jar")
    main(*sys.argv[1:])
