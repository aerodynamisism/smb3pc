# smb3pc

A native PC port of Super Mario Bros. 3 (US, PRG1). The 6502 game code is
translated to C++ from the SMB3 disassembly, and a small runtime supplies the
NES hardware (CPU, PPU, APU, MMC3 mapper, controllers). Graphics are presented
with Vulkan and sound goes through SDL3.

**You must supply your own legally obtained ROM.** Nothing from the ROM is
committed to this repository. The build reads the ROM, checks it, and extracts
the PRG and CHR data into `assets/` (gitignored).

## Requirements

- CMake 3.20+, Ninja or Make, a C++17 compiler, Python 3, git
- Vulkan SDK or driver headers, `glslc` (for the shaders)
- SDL3 (found through pkg-config)
- `Super Mario Bros. 3 (U) (PRG1) [!].nes` in the repo root, or set
  `-DSMB3_ROM=/path/to/rom.nes`

Without SDL3, Vulkan or glslc the desktop app is skipped. The headless
verification tool still builds.

## Get the disassembly

The 6502 source is not vendored. Clone it into `third_party/smb3/` first:

```
git clone https://github.com/captainsouthbird/smb3 third_party/smb3
```

## Build

```
cmake -S . -B build -G Ninja
cmake --build build
```

The build runs three steps: it extracts the assets from your ROM, translates
the disassembly to C++ (`build/gen/`), and compiles the runtime. The translator
checks every item against the ROM.

## Run

```
./build/smb3pc
```

| Action | Keyboard | Gamepad |
| --- | --- | --- |
| D-pad | Arrow keys | D-pad |
| A | X | South (A/Cross) |
| B | Z | East (B/Circle) |
| Select | Right Shift | Back |
| Start | Enter | Start |
| Menu (fullscreen, FPS counter) | F11 | |
| Quit (closes the menu first, if open) | Esc | |

F11 opens a menu over the game and pauses it. Up and Down move the cursor,
Enter or X selects (D-pad and South on a gamepad). Select Fullscreen to toggle
full screen, FPS counter to show the emulated frame rate, or Resume to close it.

Picture fills the window with square pixels: it is zoomed to cover the window, and the edges that overflow are trimmed.

### Remapping

Copy `smb3pc.cfg.example` to `smb3pc.cfg` and edit it. Key names and gamepad
button names are SDL's. An unknown name is reported and the program exits.
Use `--config FILE` to load a different file.

## Headless check

```
./build/smb3_headless --assets assets --frames 600 --out frame.png
```

This runs the game for N frames with no window and writes the last frame as a
PNG.

## Status

Verified:

- The disassembly reassembles byte-identical to the ROM, and the translator
  checks all 73,877 items against it with zero mismatches.
- The headless runner executes the translated game. CPU timing is about 29,780
  cycles per NTSC frame, and NMI fires every frame.
- The desktop app builds, runs with the Khronos validation layer, and reports
  no Vulkan errors. A bad remap file is rejected with a clear message.
- The APU output has no DC offset, and pitch is correct for pulse 1 (440 Hz)
  and the triangle (3.5 kHz). The output passes the NES high-pass (90 Hz, 440 Hz)
  and low-pass (14 kHz) filters. Output is 48 kHz from a band-limited resampler
  (windowed-sinc steps), so high-frequency content does not alias. The audio device
  paces the emulation, so no audio is dropped.
- The picture is cropped by 8 pixels on each side (overscan), leaving 240 of 256
  columns.
- The DMC plays a sample from CPU memory and consumes every byte. Its CPU stall
  is not modelled.
- The game writes the APU (`$4015` and the others) and produces non-silent audio
  over 2400 frames of title and map screens.

Not yet verified or not working:

- **The APU is written from the NESdev hardware spec, not from another emulator.**
  It is not bit-exact with Mesen or hardware. Output has not been compared
  against a reference recording.
- **Audio has not been listened to.** The checks are numeric (pitch, DC, byte
  consumption, RMS).
- The DMC IRQ is not emulated, and DMC sample reads use the bank mapped when the
  audio is rendered, not when the write happened.
- **The on-screen picture has not been checked by eye.** The headless frames
  have real content (10 colours at frames 500 and 1200), but I could not view
  the window or a PNG in this environment.
- The CPU renderer's sprite priority, the 8-sprite limit, sprite-0 hit and the
  scroll split are written but not individually tested.
- The gamepad path was not tested on hardware.
- Noise, envelope and sweep in the APU are not individually tested.
- Audio is not used to pace frames; the frame timer is. Audio may underrun or
  drift on a slow machine.

## Layout

- `src/runtime/` the NES runtime (CPU helpers, memory map, MMC3, PPU, APU, palette)
- `src/app/` the desktop app (SDL3 window, Vulkan presenter, input)
- `src/app/shaders/` the GLSL shaders, compiled with `glslc` at build time
- `src/tools/headless.cpp` the headless runner
- `tools/extract/` ROM checks and asset extraction
- `tools/translate/` 6502 disassembly to C++ translator
- `third_party/smb3/` the disassembly (fetched, not vendored)

## Credits

- The SMB3 disassembly, which this port is built from:
  [captainsouthbird/smb3](https://github.com/captainsouthbird/smb3). It
  reassembles to the US PRG1 ROM with NESASM.
- Nintendo created Super Mario Bros. 3. This project is not affiliated with
  or endorsed by Nintendo.
