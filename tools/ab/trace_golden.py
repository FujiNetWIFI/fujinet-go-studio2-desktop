#!/usr/bin/env python3
"""trace_golden.py -- fold a bus trace (tools/ab/abtrace.lua's, or
studio2_headless -trace) into the count and FNV-1a 64 hash that
core/tests/mame_trace_test.c holds the core to.

    tools/ab/trace_golden.py TRACE LO HI

Only reads with LO <= cycle < HI count. Each line is hashed as written
("cycle addr data\\n", cycle in decimal, addr and data in upper-case hex).
See tools/ab/README.md for how the goldens were made.
"""
import sys


def main():
    path, lo, hi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    h, n = 0xcbf29ce484222325, 0
    for line in open(path):
        cyc = int(line.split()[0])
        if lo <= cyc < hi:
            for ch in line.encode():
                h = ((h ^ ch) * 0x100000001b3) & 0xffffffffffffffff
            n += 1
    print("%d reads, hash 0x%016XULL" % (n, h))


if __name__ == "__main__":
    main()
