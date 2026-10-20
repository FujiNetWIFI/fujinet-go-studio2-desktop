#!/usr/bin/env python3
"""mame_ab.py -- frame-for-frame A/B of this app's core against MAME.

    tools/ab/mame_ab.py [--bios FILE] [--cart IMAGE | --nocart | --config]
                        [--frames N] [--keys F:P:K:D,...] [--mame-power-on]
                        [--out DIR]

Side A is MAME's own studio2 driver: ~/Workspace/mame-studio2/studio2 (a
studio2 build carrying the firmware's FujiNet graft, pico/studio2/emu),
running tools/ab/abdump.lua. With --cart, side A is stock MAME's cart (the
FujiNet device inert) and side B serves the same image through the FujiNet
cart (DIRECT), as the firmware's Tier A does; add --direct to serve it
through MAME's FujiNet device on side A too, which compares the cores
alone. --config runs the baked CONFIG on both sides with the link down;
--nocart is the console alone (the BIOS's built-in games). Side B is
build*/core/tests/studio2_headless. Use --mame-power-on for any run with
the real BIOS: MAME's CPU skips the power-on CLEAR reset (machine.c).

Frames are compared as pictures (lit or not). MAME's first frame-notifier
call can come a frame ahead of ours, so the sequences are aligned on the
offset (0..3) that matches longest. Prints the first mismatch and writes a
side-by-side PNG of it (Pillow) to --out.

The BIOS is the user's own (MAME's studio2.zip): it is never put in the
repository; point --bios at a copy outside it.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAME_DIR = os.environ.get("MAME", os.path.expanduser("~/Workspace/mame-studio2"))
MAME_BIN = os.environ.get("MAMEBIN", "./studio2")
ROMS = os.environ.get("ROMS", os.path.expanduser("~/Workspace/mame/roms"))
W, H = 100, 246


def find_headless():
    for d in sorted(os.listdir(ROOT)):
        p = os.path.join(ROOT, d, "core", "tests", "studio2_headless")
        if d.startswith("build") and os.access(p, os.X_OK):
            return p
    sys.exit("mame_ab: no build*/core/tests/studio2_headless; build it first")


def frames_of(path, bpp):
    data = open(path, "rb").read()
    size = W * H * bpp
    out = []
    for i in range(len(data) // size):
        chunk = data[i * size:(i + 1) * size]
        if bpp == 4:
            out.append(bytes(1 if (chunk[j] | chunk[j + 1] | chunk[j + 2]) else 0
                             for j in range(0, size, 4)))
        else:
            out.append(chunk)
    return out


def save_pair(path, a, b):
    try:
        from PIL import Image
    except ImportError:
        return
    im = Image.new("RGB", (2 * W + 4, H), (255, 0, 0))
    for k, fr in enumerate((a, b)):
        side = Image.frombytes("L", (W, H), bytes(255 if p else 0 for p in fr))
        im.paste(side.convert("RGB"), (k * (W + 4), 0))
    im.resize((im.width * 3, im.height * 2), Image.NEAREST).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--cart")
    g.add_argument("--nocart", action="store_true")
    g.add_argument("--config", action="store_true")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--keys", default="")
    ap.add_argument("--direct", action="store_true",
                    help="side A serves --cart through MAME's FujiNet device too ($FUJINET_IMAGE), "
                         "not stock MAME's cart: the cores alone are compared")
    ap.add_argument("--key-shift", type=int, default=-2,
                    help="side B applies each key event this many frames earlier; "
                         "-2: Lua sets the field after this frame_update's input update, and the next applies it")
    ap.add_argument("--show", type=int, action="append", default=[],
                    help="also write the pair at this B frame")
    ap.add_argument("--keyline", type=int, default=0,
                    help="side B applies a frame's key events at this line of it")
    ap.add_argument("--mame-power-on", action="store_true",
                    help="side B without the CLEAR pulse (MAME's IE = 0 power-on)")
    ap.add_argument("--out", default=tempfile.gettempdir())
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="s2ab-")
    a_out = os.path.join(tmp, "a.raw")
    b_out = os.path.join(tmp, "b.raw")

    env = {k: v for k, v in os.environ.items() if not k.startswith("FUJINET")}
    env.update(AB_OUT=a_out, AB_FRAMES=str(args.frames + 4), AB_KEYS=args.keys)
    cmd = [MAME_BIN, "studio2", "-rompath", ROMS, "-skip_gameinfo", "-video", "none",
           "-sound", "none", "-nothrottle", "-autoboot_script",
           os.path.join(ROOT, "tools", "ab", "abdump.lua")]
    if args.cart and args.direct:
        env["FUJINET_IMAGE"] = os.path.abspath(args.cart)
    elif args.cart:
        cmd += ["-cart", os.path.abspath(args.cart)]
    if args.config:
        env.update(FUJINET="1", FUJINET_TCP="127.0.0.1:1")
    subprocess.run(cmd, cwd=MAME_DIR, env=env, capture_output=True, timeout=600)

    bcmd = [find_headless(), "-frames", str(args.frames), "-out", os.path.join(tmp, "b"),
            "-dump", b_out, "-sync", "-boip", "127.0.0.1:1"]
    if args.bios:
        bcmd += ["-bios", args.bios]
    if args.cart:
        bcmd += ["-cart", args.cart]
    if args.nocart:
        bcmd.append("-nocart")
    if args.mame_power_on:
        bcmd.append("-mame")
    if args.keyline:
        bcmd += ["-keyline", str(args.keyline)]
    for ev in filter(None, args.keys.split(",")):
        f, p, k, d = ev.split(":")
        f = max(0, int(f) - args.key_shift)
        if p == "C":
            bcmd += ["-clear", "%d:%s" % (f, d)]
        else:
            bcmd += ["-key", "%d:%s:%s:%s" % (f, p, k, d)]
    subprocess.run(bcmd, env={k: v for k, v in os.environ.items() if not k.startswith("FUJINET")},
                   capture_output=True, timeout=600)

    A = frames_of(a_out, 4)
    B = frames_of(b_out, 1)
    if not A or not B:
        sys.exit("mame_ab: a side produced no frames (A %d, B %d)" % (len(A), len(B)))
    best = (-1, 0)
    for off in range(0, 4):
        n = 0
        while n < len(B) and n + off < len(A) and A[n + off] == B[n]:
            n += 1
        if n > best[0]:
            best = (n, off)
    n, off = best
    total = min(len(B), len(A) - off)
    lit = sum(1 for fr in B if any(fr))
    print("aligned at +%d: %d/%d frames equal (%d of B's frames lit)" % (off, n, total, lit))
    bad = [i for i in range(total) if A[i + off] != B[i]]
    if bad:
        ranges, start = [], bad[0]
        for a, b in zip(bad, bad[1:] + [None]):
            if b != a + 1:
                ranges.append("%d" % start if start == a else "%d-%d" % (start, a))
                if b is not None:
                    start = b
        print("%d frame(s) differ: %s" % (len(bad), " ".join(ranges[:20])))
    for fr in args.show:
        if fr < total:
            save_pair(os.path.join(args.out, "s2ab-frame-%05d.png" % fr), A[fr + off], B[fr])
    if n < total:
        path = os.path.join(args.out, "s2ab-mismatch-%05d.png" % n)
        save_pair(path, A[n + off], B[n])
        diff = sum(1 for x, y in zip(A[n + off], B[n]) if x != y)
        print("first mismatch at B frame %d (%d pixels differ): %s" % (n, diff, path))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
