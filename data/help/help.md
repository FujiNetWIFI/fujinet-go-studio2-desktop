This is the text of FujiNet Go Studio II's built-in Help (Help ▸ FujiNet Go
Studio II Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to
the frontend it is about; here you see them all. Every topic below is one page
in the app.

# Getting started

FujiNet Go Studio II is an RCA Studio II with a FujiNet cartridge in its slot.
The console is MAME's: the CDP1802 COSMAC processor, the CDP1861 Pixie video
chip and the beeper. The FujiNet runs inside the app, so CONFIG comes up at
once and can load games from the network.

1. **Start the app.** The console powers on and runs FujiNet's CONFIG. It
   needs no BIOS for that: the app has a small stand-in of its own. See
   [BIOS and ROMs](#bios-and-roms).
2. **Pick a game in CONFIG.** Browse from the keypad: choose a host (the SD
   folder or a server on the network) and an image on it. FujiNet pushes it to
   the cartridge, CONFIG arms the swap and starts the console over, and the
   cartridge swaps the image in. See [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…**
   (<kbd>Ctrl+O</kbd>) takes a `.st2`, `.bin` or `.rom` image. See
   [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Keypad window (<kbd>F9</kbd>) work
   the two keypads and CLEAR. See [Controllers and keys](#controllers-and-keys).
5. **Change the console.** [Preferences](#preferences) picks the BIOS, the
   picture, the sound and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Keypad window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-studio2-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…**, **Eject Cartridge**, **Import Cartridge to SD…** and
  **Import BIOS…**: see [Cartridges](#cartridges) and
  [BIOS and ROMs](#bios-and-roms).
- **CLEAR** and **Reset to CONFIG**.
- **Keypad** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **TV Aspect**, **Smooth Scaling** and **Fullscreen** (<kbd>F11</kbd>).
- **FujiNet Web UI** and **Console Log**.
- **Preferences** (<kbd>Ctrl+,</kbd>), **Help**, **About** and **Quit**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, Console, View, FujiNet and Help. The Machine menu
has the cartridge and BIOS items, Preferences (Settings on Windows) and Quit
(Exit on Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences (called Settings) and Quit are in the app menu, and the
cartridge and BIOS items are in the Machine menu.

<!-- end -->

The status shows the cartridge and FujiNet's state:

| Status | Means |
|---|---|
| connected | The cartridge is talking to FujiNet. |
| link down | FujiNet is off or not answering yet; CONFIG tries again. |
| loading 42% | FujiNet is sending a program to the cartridge. |
| game running; mailbox closed | A game that does not use FujiNet is running. |

A **Needs BIOS** message (with an **Import BIOS…** button) appears when a game
or a game booted from CONFIG needs the console's own BIOS and has none. See
[Needing the BIOS](#needing-the-bios). Other messages (a cartridge opened, a
gamepad plugged in, FujiNet restarted) show for a few seconds over the picture.

## Reset and power

- **CLEAR** (<kbd>F3</kbd>) is the console's CLEAR key, which holds the CPU in
  reset and resets the 1861: the cartridge restarts whatever it serves.
- **Reset to CONFIG** (<kbd>Ctrl+R</kbd>, or <kbd>Escape</kbd>) is a power
  cycle: on the hardware only losing power brings CONFIG back. It ejects an
  opened cartridge and boots CONFIG again.

## The picture

The console draws a 100 × 246 picture. **TV Aspect** shows it with a
television's shape, or square. **Smooth Scaling** blends the pixels when the
picture is stretched, and leaves them sharp when it is off.

# Controllers and keys

The Studio II has two 10-key keypads, **A** and **B** (1 2 3 / 4 5 6 / 7 8 9 /
0), and the console's **CLEAR** key. There are no joysticks: games use the
keys.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Keypad | Keys |
|---|---|
| A, 1 2 3 | <kbd>Q</kbd> <kbd>W</kbd> <kbd>E</kbd> |
| A, 4 5 6 | <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> |
| A, 7 8 9 | <kbd>Z</kbd> <kbd>X</kbd> <kbd>C</kbd> |
| A, 0 | <kbd>Left Alt</kbd> |
| B, 1 2 3 | <kbd>Keypad 7</kbd> <kbd>Keypad 8</kbd> <kbd>Keypad 9</kbd> |
| B, 4 5 6 | <kbd>Keypad 4</kbd> <kbd>Keypad 5</kbd> <kbd>Keypad 6</kbd> |
| B, 7 8 9 | <kbd>Keypad 1</kbd> <kbd>Keypad 2</kbd> <kbd>Keypad 3</kbd> |
| B, 0 | <kbd>Keypad 0</kbd> |

| Console | Key |
|---|---|
| CLEAR | <kbd>F3</kbd> |
| Reset to CONFIG | <kbd>Escape</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+O</kbd> | Open Cartridge |
| <kbd>Ctrl+R</kbd> | Reset to CONFIG |
| <kbd>Ctrl+,</kbd> | Preferences |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Keypad |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde windows -->

Keys with <kbd>Ctrl</kbd> never reach the console.

<!-- end -->

<!-- only: macos -->

Keys with ⌘ never reach the console.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. A pad steers with the keys the games use: 2 4 6 8, the diagonals on 1 3 7
9, fires with 0 and 5, and Start is CLEAR. A pad with no standard layout (a
plain USB adapter) uses its raw buttons and hats. With **Analog sticks steer
too** on, the left stick does too.

## Changing the controls

The controls are changed in the Keypad window (<kbd>F9</kbd>): click **Map**,
then click a key, then press the keyboard key or the gamepad button to give it.
The control's old key or button is taken from it. **Defaults** puts every
control back. The map is kept in the settings, and only what differs from the
defaults is written.

<!-- context: keypad -->
# Keypad window

The Keypad window (<kbd>F9</kbd>) shows both keypads live, with CLEAR, and
presses their keys with the mouse. It floats on its own at a fixed size. The
keys that are held light up in the accent colour, whoever holds them: the
keyboard, a gamepad or the mouse.

| With the mouse | Does |
|---|---|
| Press a keypad key | Holds it for as long as the button is down |
| Press CLEAR | Holds it |

The console samples its keys once a frame, so a key is *held*, not pulsed, as
the plastic one is. Switching to another window lets go of everything the mouse
holds. Keys typed in the Keypad window go to the console. **Map** and
**Defaults** change the controls (see
[Changing the controls](#changing-the-controls)).

# Machines

There is one machine: the RCA Studio II, NTSC, with the CDP1802 COSMAC at
1.76 MHz and 59.98 frames a second.

# BIOS and ROMs

The console's BIOS is built into the app (it is in the public domain), so
commercial games, which are BIOS byte-code, run out of the box. CONFIG and every
FujiNet app need no BIOS at all.

## The stand-in

With **Use the BIOS** off, CONFIG and every FujiNet app boot on the app's own
92-byte stand-in: it clears RAM, turns the display on and enters the app at
`$0402`, as the BIOS would. Games do not run on it.

## Importing it

**Import BIOS…** (in the Machine menu, on the Machine page of Preferences, and
in the Needs BIOS message) takes your own dump in place of the built-in one:
MAME's 2K set, the 1K system ROM, or the four 512-byte ROMs. Each is identified by its size and CRC-32,
whatever it is called. Dropping the file on the window does the same. Imported
files are copied into the app's `roms` folder (see
[Files and folders](#files-and-folders)).

<!-- context: nobios -->
## Needing the BIOS

A game opened, or booted from CONFIG, while Preferences ▸ Machine ▸ **Use the
BIOS** is off says so: the stand-in runs instead, which boots CONFIG and FujiNet
apps, not games. Turn the switch on and reset to CONFIG.

# Cartridges

A cartridge is a file: a `.st2`, `.bin` or `.rom`. Every image (CONFIG, a game
booted over the network, a local file) is planned by the cartridge's own `s2map`
engine, so what works here works on the cartridge: an ST2 of up to 192 blocks,
or a raw image of up to 1K at `$0400`. An image it cannot plan is refused with
the reason.

- **A FujiNet app** carries the `FUJI` claim at `$07FC` and keeps the cartridge's
  mailbox and text engine: it can talk to the network.
- **A game** runs alone.

## Open Cartridge

**Open Cartridge…** (<kbd>Ctrl+O</kbd>) serves a local image from power-on in
place of CONFIG. It is remembered and starts again next time; **Eject
Cartridge** or **Reset to CONFIG** forgets it. Dropping a cartridge file on the
window does the same; a BIOS is imported, and anything else goes to the SD
folder.

<!-- context: import-sd -->
## Import Cartridge to SD

**Import Cartridge to SD…** copies a cartridge into FujiNet's SD folder, so
CONFIG can list it and boot it through the cartridge. The cartridge refuses
what it cannot serve, and the message says why. A game that boots from the SD
folder still needs the console's BIOS.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. It sees every console
read, display DMA included, and claims whole pages outside the BIOS: it serves
the raster the 1861 fetches, a 16 × 10 text engine drawn into it, and the
mailbox at $E400. The cartridge dials FujiNet over a local socket (BoIP,
FujiBus over TCP), and keeps trying again if the link drops.

## CONFIG

CONFIG is FujiNet's program for choosing what to run, driven from the keypad:

- **Hosts** are where games come from: the SD folder, and servers on the
  network.
- Pick a host and an image on it. FujiNet pushes the file to the cartridge,
  which plans and stages it beside CONFIG; CONFIG arms the swap and starts the
  console over, and the cartridge swaps the image in on the first fetch of
  `$0000`.
- **Reset to CONFIG** brings CONFIG back.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11519/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## Turning it off

Preferences ▸ FujiNet ▸ **Enable FujiNet** turns the FujiNet off. The cartridge
then reports *link down*, CONFIG runs without hosts, and nothing is dialled.
Games are not affected.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet and
dials another FujiNet instance at the address you give, for example
`192.168.1.20:11518`. Leave the field empty for `127.0.0.1:11518`. Start that
instance listening there (its `[BOIP]` port). FujiNet Web UI and Console Log
belong to the built-in FujiNet, so they say the FujiNet is external; use the
other instance's own. The Debugger's FujiNet tab still shows the traffic.

## Behind the scenes

- The cartridge and FujiNet talk over TCP port 11518 on 127.0.0.1 (the
  BoIP port), and the web UI is on port 11519. Nothing from outside can
  reach either.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the console. It is one page of groups.
The BIOS choice starts a new console when Preferences closes: an opened
cartridge boots again. The picture and the volume apply at once; the FujiNet
settings apply by restarting the session. The settings are one file, shared by
the GNOME and KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu).

<!-- end -->

<!-- only: windows -->

On Windows this window is called **Settings** (in the Machine menu).

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **BIOS**: built in or imported, with an **Import BIOS…** button.
- **Use the BIOS**: off, the app's own stand-in runs instead. It boots
  CONFIG and FujiNet apps, not games (setting `use_bios`).

## Display settings

- **Aspect**: *Television* or *Square pixels* (setting `aspect`).
- **Smooth scaling**: blend the pixels when the picture is stretched (setting
  `smooth`).

## Audio settings

- **Enable audio**: open the system audio device (setting `enable_audio`).
- **Volume**: the output level, 0 to 100 (setting `volume`).

## Input settings

- **Enable gamepads**: poll USB and Bluetooth gamepads (setting
  `enable_gamepad`).
- **Analog sticks steer too**: the left stick moves like the D-pad (setting
  `analog_joystick`).
- **Gamepads**: each one plugged in, and which keypad it drives. A new gamepad
  drives keypad A, the next keypad B; choose *Keypad A* or *Keypad B* to fix
  it, or *Automatic*. The keys and buttons are changed in the Keypad window
  (see [Changing the controls](#changing-the-controls)).

## FujiNet settings

- **Enable FujiNet**: run the built-in FujiNet (setting `enable_fujinet`).
- **Use external FujiNet**: dial another instance instead (setting
  `fujinet_external`).
- **Address**: its `host:port`, or just a port (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `cart` (the opened cartridge,
empty for CONFIG) and `bindings` (the controls that differ from the defaults).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has eight tabs; closing it lets the console go on as it was.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a `SEP` call or an SCRT call |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Scanline+1 | On to the next scanline |
| Frame+1 | On to the next frame |
| Help | Opens the topic of the tab that shows |

<!-- context: debugger/prompt -->
## Prompt

The Prompt takes commands, with completion on <kbd>Tab</kbd>. Conditions run
over registers, flags, memory and the beam. `help` lists the commands.

| Command | Does |
|---|---|
| `break` | An execute breakpoint |
| `bpr`, `bpw` | Memory reads or writes |
| `bpin`, `bpout` | I/O port reads or writes |
| `print` | Evaluates an expression |
| `mem`, `poke` | Dumps memory; writes bytes |
| `video` | The 1861's state |
| `io`, `keys` | The ports; the keypads |
| `cart`, `mailbox` | The cartridge, and FujiNet's mailbox |
| `text` | The text engine's screen |
| `label` | Name an address |
| `runto` | Runs to an address |
| `disasm` | Disassembles |
| `trace` | The instruction trace |
| `save` | Saves state to a file |

<!-- context: debugger/cpu -->
## CPU and RAM

R0 to RF, D, DF, P, X, T, B, IE, Q and EF1 to EF4, and the 512 bytes of RAM,
editable. Click a value to edit it while stopped.

<!-- context: debugger/disassembly -->
## Disassembly

The 1802 disassembled at the PC in MAME's mnemonics, with the opcode bytes,
labels and the breakpoints in the margin. Click a line to toggle its
breakpoint. Follow PC keeps the current line in view. Symbols load from
Macroassembler AS `.map` and listing files, WLA-DX `.sym` and plain lists; the
mailbox, the text engine, the raster and the clients' entry are labelled out of
the box.

<!-- context: debugger/video -->
## Video

The 1861's INT, EF and DMA lines and the beam; the whole raster, the
cartridge's raster, and the BIOS display page.

<!-- context: debugger/io -->
## Sound and I/O

Q (the beeper), the key latch, both keypads and the ports.

<!-- context: debugger/breakpoints -->
## Breakpoints

The list of breakpoints with their conditions. **Add**, **Remove**, and the
check box to turn one off.

<!-- context: debugger/cart -->
## Cart

The 256-page map of what the cartridge serves, its staging and the swap, and
the text engine's screen.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet on the cartridge's link, both ways, with hex, from
power-on. **Clear** empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-studio2/settings.ini` and
the data is in `~/.local/share/fujinet-go-studio2` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-studio2\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-studio2`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-studio2/settings.ini` and
the data is in `~/.local/share/fujinet-go-studio2`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported BIOS |
| `carts` | cartridges dropped on the window |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **This game needs the BIOS**: it is a game, and no BIOS is imported (or Use
  the imported BIOS is off). See [Needing the BIOS](#needing-the-bios).
- **link down**: FujiNet is off, not started yet, or the external address
  is wrong. CONFIG tries again; check Preferences ▸ FujiNet.
- **The cartridge was refused**: the message says why: too large (an ST2 of
  more than 192 blocks, or a raw image of more than 1K), empty, or not a
  cartridge image.
- **Import Cartridge to SD refused a game**: the FujiNet cartridge cannot
  serve it; open it with Open Cartridge instead.
- **Keys don't reach the console**: a dialog or another window is in front,
  or the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it may not be on the keypad you expect (see
  [Gamepads](#gamepads)), or its buttons may be given other jobs.
- **No sound**: the audio device could not be opened, or the volume is 0.
- **A port is taken**: another program is using 11518 or 11519. Close the
  other FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go Studio II is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **MAME** (the RCA Studio II driver, the CDP1802 COSMAC and the CDP1861 Pixie,
  GPL-2.0-or-later and BSD-3-Clause) is what the console is modelled on.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own sources are the firmware's.
- No BIOS or other copyrighted ROM is part of the app; you import your own.
  "RCA" and "Studio II" are trademarks of their owners, used to name the machine
  emulated; this project is not affiliated with them.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-studio2-desktop/issues)
