#!/usr/bin/env python3
"""Assert that a frontend executable carries the RCA Studio II BIOS.

The BIOS (tools/roms/, public domain: COMPLIANCE.md) is compiled into every
frontend, so each of the four ROM images must be found whole in the binary.

usage: bios_embedded.py <binary> <rom-dir>
"""

import sys
from pathlib import Path

ROMS = ("84932.ic11", "84933.ic12", "85456.ic13", "85457.ic14")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).read_bytes()
    romdir = Path(sys.argv[2])
    missing = [n for n in ROMS if (romdir / n).read_bytes() not in binary]
    for n in missing:
        print(f"FAIL: {n} is not embedded in {sys.argv[1]}")
    if missing:
        return 1
    print(f"bios_embedded: all {len(ROMS)} ROM images embedded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
