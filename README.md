# Super Mario Bros. 3: PC Remastered

A native PC port of Super Mario Bros. 3 (US, PRG1) — the 6502 game code,
translated to C++ from the SMB3 disassembly, running on a small NES runtime
with Vulkan video and SDL3 audio.

> You must supply your own legally obtained ROM. Nothing from the ROM is
> committed to this repository. The build reads the ROM, checks it, and
> extracts the PRG and CHR data into `assets/` (gitignored).

---

## Features

**Emulation core**

- Translated 6502 game code, verified item-by-item against the ROM
- 6502 CPU state, memory map, and MMC3 mapper (PRG/CHR banking, mirroring, IRQ)
- PPU pipeline with NMI-per-frame timing (~29,780 CPU cycles per NTSC frame)
- Status-bar split handling via per-line scroll/palette/bank capture

**Video**

- Vulkan presenter with nearest-filtered fullscreen triangle
- Square pixels, fitted inside the window with pillarbox/letterbox
- Overscan crop: 8 pixels per side, leaving 240 of 256 columns
- On-screen menu and FPS counter drawn into the frame with the game's own
  font tiles (white text with black outline, no text library)

**Audio**

- 2A03 APU on Blargg Nes_Snd_Emu 0.1.7, linear mixing (no compression) through one `Blip_Buffer` per voice set
- Music and sound effects run on two separate voice sets (`Machine::apu` and
  `Machine::sfx_apu`) summed at full level at the output, so an effect never silences or retunes
  a music channel. The sum is clamped to 16-bit range only. Generated code routes each APU write by owner (translate.py,
  `SND_ROUTINE_OWNER`).
- Deliberate deviation from the ROM: the four music gates that skipped music notes
  while a sound effect was on the same channel are removed (`GATE_TARGETS` in
  translate.py). The translator reports them and checks each one.
- 48 kHz mono int16 output over SDL3; cycle-stamped register writes
- Output-rate nudge (±0.5%) holds the device queue at its target
- DMC fetches sample bytes from CPU memory

**Input**

- Keyboard and gamepad (SDL3), with hot-plug support
- Remappable via `settings.cfg`; unknown names fail fast with a clear message
- In-game menu: fullscreen toggle, FPS counter (pauses the game)

**Tools**

- Headless runner: fixed frame counts, scripted Start press, PNG + WAV output
- Asset extractor and 6502-to-C++ translator, both ROM-checked at build time

---

## Requirements

- CMake 3.20+, Ninja or Make, a C++17 compiler, Python 3, git
- Vulkan SDK or driver headers, `glslc` (for the shaders)
- SDL3 (found through pkg-config)
- `Super Mario Bros. 3 (U) (PRG1) [!].nes` or
  `Super Mario Bros. 3 (USA) (Rev 1).nes` in the repo root, or set
  `-DSMB3_ROM=/path/to/rom.nes`

Without SDL3, Vulkan, or glslc the desktop app is skipped. The headless
verification tool still builds.

## Disassembly source

The 6502 source this port is translated from is vendored under
`tools/translate/smb3-src/` (`smb3.asm` plus the `PRG/` sources it includes;
textures, editor files, and binaries stripped). It comes from
[captainsouthbird/smb3](https://github.com/captainsouthbird/smb3) at commit
`09b1bd81`. No extra clone step is needed.

## Build

```
cmake -S . -B build -G Ninja
cmake --build build
```

The build runs three steps: it extracts the assets from your ROM, translates
the disassembly to C++ (`build/gen/`), and compiles the runtime. The
translator checks every item against the ROM.

---

## Run

```
./build/smb3pc [--assets DIR] [--shaders DIR] [--config FILE]
```

| Action | Keyboard | Gamepad |
| --- | --- | --- |
| D-pad | Arrow keys | D-pad |
| A | X | South (A/Cross) |
| B | Z | East (B/Circle) |
| Select | Right Shift | Back |
| Start | Enter | Start |
| Menu (fullscreen, FPS counter) | F11 | L3 + R3 |
| Quit | Esc | — |

F11 (or L3+R3 on a gamepad) opens a menu over the game and pauses it.
Up and Down move the cursor; Enter or X selects (D-pad and South on a
gamepad). East or Start closes the menu on a gamepad; Esc quits the program.
Menu items: Fullscreen, FPS counter.
On/off states show check/cross icons; the selected row gets a red-orange
arrow cursor. The menu is drawn with the game's own font tiles inside a
world-map status strip frame: white stroked text on tan with white trim.
Close the menu with F11 (L3+R3 on a gamepad).

Picture keeps square pixels and fits inside the window: it is scaled to fill
as much as possible, and any leftover area is black (pillarbox/letterbox).
Nothing is cropped by the window fit. The startup window is 720x720, exactly
3x the visible 240x240 picture.

### Settings

Menu choices and controller remapping share `settings.cfg` in the working
directory (gitignored). It is written every time a menu item is toggled, so
settings survive even a crash; bindings you add by hand are preserved. A
missing file keeps the defaults; a bad menu value warns and keeps that key's
default, while a bad binding name or value is reported and the program exits.
Use `--config FILE` to load a different file.

| Key | Default | Meaning |
| --- | --- | --- |
| `video.fullscreen` | `0` | Start in fullscreen (`0`/`1`) |
| `video.show_fps` | `0` | Show the emulated frame rate (`0`/`1`) |
| `key.BUTTON` | (see below) | Keyboard remap, e.g. `key.a = X`; names are SDL's |
| `pad.BUTTON` | (see below) | Gamepad remap, e.g. `pad.a = south`; names are SDL gamepad buttons |

Buttons: `a`, `b`, `select`, `start`, `up`, `down`, `left`, `right`. Defaults:
keyboard arrows = D-pad, `X` = A, `Z` = B, Right Shift = Select, Enter = Start;
gamepad South = A, East = B, Back = Select, Start = Start, D-pad = D-pad.

---

## Headless checks

```
./build/smb3_headless --assets assets --frames 600 --out frame.png
./build/smb3_headless --assets assets --frames 600 --press-start 100 --out frame.png --wav out.wav
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--assets DIR` | `assets` | Asset pack with `prg.bin` (256 KB) + `chr.bin` (128 KB) |
| `--frames N` | `300` | Frames to run with no window |
| `--press-start N` | off | Hold Start for 6 frames starting at frame N (`0x08`) |
| `--out FILE` | `frame.png` | Final 256x240 frame as PNG (stored-deflate, no zlib) |
| `--wav FILE` | off | Full-run mono int16 WAV at the APU output rate |

The runner prints the frame count, output path, and CPU state
(`pc`, `cyc`). APU writes are applied and advanced inside `render()` every
frame, exactly as in the desktop app.


---

## Layout

- `src/runtime/` — NES runtime (CPU helpers, memory map, MMC3, PPU, APU, palette)
- `src/app/` — desktop app (SDL3 window, Vulkan presenter, input)
- `src/app/shaders/` — GLSL shaders, compiled with `glslc` at build time
- `src/tools/headless.cpp` — headless runner
- `tools/extract/` — ROM checks and asset extraction
- `tools/translate/` — 6502 disassembly to C++ translator (vendored source subset in `tools/translate/smb3-src/`)

## Credits

- The SMB3 disassembly, which this port is built from:
  [captainsouthbird/smb3](https://github.com/captainsouthbird/smb3). It
  reassembles to the US PRG1 ROM with NESASM.
- The APU core, Shay Green's Nes_Snd_Emu (used here in linear-mixing
  configuration through a single `Blip_Buffer`; LGPL 2.1, see
  `src/runtime/nes_apu/LGPL.txt`).
- The PPU, modelled after Ulf Magnusson's
  [nesalizer](https://github.com/ulfalizer/nesalizer), with fetch sequencing
  cross-checked against Sour's [Mesen](https://github.com/nesdev-org/MesenCE)
  (`NesPpu`; rewritten, no Mesen code).
- Nintendo created Super Mario Bros. 3. This project is not affiliated with
  or endorsed by Nintendo.
