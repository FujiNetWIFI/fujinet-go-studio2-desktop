# tools/ab — the core against MAME

All of this runs locally against a studio2 MAME build carrying the
firmware's FujiNet graft (`fujinet-firmware/pico/studio2/emu/apply.sh`, then
`make SUBTARGET=studio2 SOURCES=src/mame/rca/studio2.cpp REGENIE=1`), by
default `~/Workspace/mame-studio2/studio2` (`$MAME`, `$MAMEBIN`). Side B is
the newest `build*/core/tests/studio2_headless` (`$HEADLESS`).

- `mame_ab.py` — frame for frame, the same scripted keys on both sides,
  pictures compared lit/dark. `--nocart` (the console alone), `--cart IMAGE`
  (stock MAME's cart vs the FujiNet cart; add `--direct` to serve it through
  MAME's FujiNet device too), `--config` (the baked CONFIG, link down).
  Real-BIOS runs need your own BIOS (`--bios`, never in the repo),
  `--mame-power-on`, and Clear pressed first (`C` in `--keys`): MAME powers
  on with IE = 0, so until Clear the BIOS's interrupt routine never runs and
  both sides only show the DMA of a stray R0. Keys from Lua land two frames
  late in MAME (`--key-shift`, default -2). `--wav` also compares the
  beeper, sample by sample (MAME's `-wavwrite`, the headless `-wav`).
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

## The real-BIOS sweep

Every TOSEC game served DIRECT, and the built-in games, with Clear first
and keys on both pads:

```sh
K="10:C:0:1,14:C:0:0"
for f in $(seq 100 25 1100); do
  k=$(( (f/25) % 10 )); p=$([ $(( (f/250)%2 )) = 0 ] && echo A || echo B)
  K="$K,$f:$p:$k:1,$((f+10)):$p:$k:0"
done
for g in ~/Workspace/fnpc-studio2/SD/studio2/*.st2; do
  tools/ab/mame_ab.py --bios studio2_bios.bin --cart "$g" --direct \
      --mame-power-on --frames 1200 --keys "$K" --wav
done
tools/ab/mame_ab.py --bios studio2_bios.bin --nocart --mame-power-on \
    --frames 1200 --keys "$K" --wav
```

2026-10-20: 1200/1200 frames equal for all eight games (Combat, Hockey,
Space Invaders, Tennis-Squash, Spacewar, Baseball, Speedway-Tag,
Blackjack) and the built-in games; every beep's samples agree but for
MAME's ringing on the edges and starts and ends up to two samples apart.
