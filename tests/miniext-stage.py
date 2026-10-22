#!/usr/bin/env python3
"""Generate the miniext test staging tree, and the expectations that go with it.

The tree is generated rather than checked in: it contains a 1.6 MB file whose
only purpose is to span enough extents to be interesting, and that does not
belong in git.

    test/miniext-stage.py build/miniext-stage          # write the tree
    test/miniext-stage.py build/miniext-stage --table  # and print the C table

Then:

    scripts/mkdata.sh build/data.img build/miniext-stage 64M
    make app=test && scripts/run.py --emulated-nvme build/data.img

If you change the tree, re-run with --table and paste the result over the
`expected[]` array in test/os-miniext.cc. The checksums are what prove miniext
read back exactly what mkfs.ext4 wrote.
"""

import argparse
import os
import sys


def fnv1a(data):
    """FNV-1a 64, matching the implementation in test/os-miniext.cc."""
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def build(root):
    os.makedirs(os.path.join(root, "sub"), exist_ok=True)

    files = {}

    # Small file: fits inside a single block.
    files["hello.txt"] = b"hello from miniext\n"

    # Large file: "%08d" repeated, so byte N is digit N%8 of the number N/8.
    # That makes any byte's expected value computable, which is what lets the
    # suite check unaligned positional reads without a copy of the file.
    big = bytearray()
    i = 0
    while len(big) < 1600000:
        big += b"%08d" % i
        i += 1
    files["big.bin"] = bytes(big[:1600000])

    # Size deliberately not a multiple of the 4096-byte block, so the final
    # partial block is exercised.
    files["odd.bin"] = bytes(range(256)) * 17 + b"TAIL"

    # One level down, to exercise path resolution through a directory.
    files["sub/nested.txt"] = b"nested file\n"

    for name, data in files.items():
        with open(os.path.join(root, name), "wb") as fh:
            fh.write(data)

    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="staging directory to create")
    ap.add_argument("--table", action="store_true",
                    help="print the expected[] table for test/os-miniext.cc")
    args = ap.parse_args()

    files = build(args.root)
    print(f"miniext-stage: wrote {len(files)} files to {args.root}", file=sys.stderr)

    if args.table:
        print("const expected_file expected[] = {")
        width = max(len(n) for n in files) + len("/db/") + 3
        for name, data in files.items():
            path = f'"/db/{name}",'
            size = f"{len(data)},"
            print(f"    {{{path:<{width}} {size:<9} 0x{fnv1a(data):016x}ull}},")
        print("};")


if __name__ == "__main__":
    main()
