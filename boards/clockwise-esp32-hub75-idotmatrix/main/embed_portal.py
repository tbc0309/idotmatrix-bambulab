import gzip
import sys
from pathlib import Path

source, header = map(Path, sys.argv[1:])
plain = source.read_bytes()
compressed = gzip.compress(plain, mtime=0)

def array(name, data):
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return f"static const char {name}[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"

header.write_text("#pragma once\n" + array("portal_page", plain) +
                  array("portal_page_gzip", compressed), encoding="ascii")
