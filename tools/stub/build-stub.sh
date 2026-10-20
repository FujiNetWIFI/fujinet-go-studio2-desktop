#!/usr/bin/env bash
# build-stub.sh -- assemble core/studio2/stub.asm (the no-BIOS stand-in) with
# the Macroassembler AS and print it as the C array core/studio2/stub.c
# carries. Run it after editing stub.asm and paste the array in;
# core/tests/stub_test.c checks what the stand-in does, not its bytes.
set -euo pipefail
cd "$(dirname "$0")/../.."
ASL=${ASL:-$HOME/asl/asl}
P2BIN=${P2BIN:-$(dirname "$ASL")/p2bin}
[ -x "$ASL" ] || ASL=$(command -v asl) || { echo "build-stub.sh: no asl" >&2; exit 1; }
[ -x "$P2BIN" ] || P2BIN=$(command -v p2bin) || { echo "build-stub.sh: no p2bin" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp core/studio2/stub.asm "$tmp/"
(cd "$tmp" && "$ASL" -q -U -L -o stub.p -olist stub.lst stub.asm && "$P2BIN" -q stub.p stub.bin)
python3 - "$tmp/stub.bin" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
print(f"/* {len(b)} bytes */")
for i in range(0, len(b), 12):
    print("    " + " ".join(f"0x{x:02X}," for x in b[i:i+12]))
PY
