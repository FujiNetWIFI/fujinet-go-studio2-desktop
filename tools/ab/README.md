# tools/ab — the core against MAME

All of this runs locally against a studio2 MAME build carrying the
firmware's FujiNet graft (`fujinet-firmware/pico/studio2/emu/apply.sh`, then
`make SUBTARGET=studio2 SOURCES=src/mame/rca/studio2.cpp REGENIE=1`), by
default `~/Workspace/mame-studio2/studio2` (`$MAME`, `$MAMEBIN`). Side B is
`build*/core/tests/studio2_headless`.

- `mame_ab.py` — frame for frame, the same scripted keys on both sides,
  pictures compared lit/dark. `--nocart` (the console alone), `--cart IMAGE`
  (stock MAME's cart vs the FujiNet cart; add `--direct` to serve it through
  MAME's FujiNet device too), `--config` (the baked CONFIG, link down).
  Real-BIOS runs need your own BIOS (`--bios`, never in the repo) and
  `--mame-power-on`. Keys from Lua land two frames late in MAME
  (`--key-shift`, default -2).
- `abdump.lua` — MAME side of `mame_ab.py` (every frame's pixels).
- `abtrace.lua` + `studio2_headless -trace` — every bus read with its cycle,
  both sides, for finding where two runs part.
- `trace_golden.py` — folds a trace into the count and hash that
  `core/tests/mame_trace_test.c` checks on every build, so CI holds the core
  to MAME without any RCA code.

## Regenerating the mame_trace goldens

The game run: MAME with the tests' own BIOS split into MAME's four ROM
files (`core/tests/test_rom.h`'s `test_bios`, 512 bytes each as
`84932.ic11`, `84933.ic12`, `85456.ic13`, `85457.ic14` in a scratch rompath
`<dir>/studio2/`; MAME warns about the checksums and runs) and the counter
game (`test_counter_game`) as `-cart`:

```sh
AB_TRACE=mame.trace AB_TRACE_FROM=9 AB_TRACE_TO=121 \
AB_KEYS="60:A:1:1,70:A:1:0,90:B:3:1,95:B:3:0" \
./studio2 studio2 -rompath <dir> -cart game.bin -skip_gameinfo -video none \
    -sound none -nothrottle -autoboot_script tools/ab/abtrace.lua
tools/ab/trace_golden.py mame.trace 293440 3550624
```

The CONFIG run: the stand-in (`core/studio2/stub.c`'s bytes, padded to 2K)
as the four ROM files, `FUJINET=1 FUJINET_TCP=127.0.0.1:1 FUJINET_ASYNC=0`,
no cart, `AB_TRACE_FROM=0 AB_TRACE_TO=300`, then
`trace_golden.py mame.trace 29344 8803200`.
