#!/usr/bin/env python3
"""Drop top-level C functions (and their leading comment and return-type lines) from a DPDK source file.

  prune.py <in.c> <out.cc> name...
"""
import re
import sys


def prune(lines, names):
    names = set(names)
    out, i, dropped = [], 0, set()
    while i < len(lines):
        # "name(" at column 0, or "type name(" on one line; not a prototype.
        m = re.match(r"^(?:[A-Za-z_][\w ]*[\s*])?([A-Za-z_]\w*)\(", lines[i])
        if not (m and m.group(1) in names) or lines[i].rstrip().endswith(";"):
            out.append(lines[i])
            i += 1
            continue
        # Return type and qualifiers may sit on the lines just above, then an optional comment block.
        while out and out[-1].strip() and not out[-1].startswith("}") and not out[-1].rstrip().endswith("*/"):
            out.pop()
        if out and out[-1].rstrip().endswith("*/"):
            while out and not out[-1].lstrip().startswith("/*"):
                out.pop()
            out.pop()
        # The body ends where its braces balance; DPDK does not always put the closing one in column 0.
        depth, opened = 0, False
        while i < len(lines):
            code = re.sub(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])*\'|//.*$', "", lines[i])
            depth += code.count("{") - code.count("}")
            opened = opened or "{" in code
            i += 1
            if opened and depth == 0:
                break
        while i < len(lines) and not lines[i].strip():
            i += 1
        dropped.add(m.group(1))
    missing = names - dropped
    if missing:
        sys.exit("not found: " + " ".join(sorted(missing)))
    return out


if __name__ == "__main__":
    src, dst, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    lines = open(src).read().split("\n")
    open(dst, "w").write("\n".join(prune(lines, names)))
