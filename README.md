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
- Status bar drawn as its own display layer (`src/runtime/hud.cpp`): composed once
  into a 256-wide surface in bar-local coordinates and copied over the frame, rebuilt
  only when the nametable, palette, CHR banks, OAM or split rows change, so a steady
  frame costs one state compare and one copy instead of a full redraw. The frame is two
  layers with one boundary, not a per-row reconstruction of the split: the IRQ row only
  says where the game starts writing the bar's scroll, and the first row that no longer
  continues the playfield above (other scroll, fine X or CHR bank) is where the bar
  begins. Level rows above it keep their own captured scroll, so the ground under the
  player runs to the level's real last scanline, and no row the bar owns can contain
  level pixels

**Video**

- Vulkan presenter with nearest-filtered fullscreen triangle
- Square pixels, fitted inside the window with pillarbox/letterbox
- Overscan crop: 8 pixels per side, leaving 240 of 256 columns
- Widescreen (16:9): same pixel scale and same 240 rows, widened from 256 to
  426 columns with the level content really to either side of the frame (see
  [Widescreen](#widescreen-169)); 4:3 output is unchanged
- On-screen menu and FPS counter drawn into the frame with the game's own
  font tiles (white text with black outline, no text library)

**Audio**

- 2A03 APU on Blargg Nes_Snd_Emu 0.1.7, linear mixing (no compression) through one `Blip_Buffer` per voice set
- Music and sound effects run on two separate voice sets (`Machine::apu` and
  `Machine::sfx_apu`) summed at full level at the output, so an effect never silences or retunes
  a music channel. The sum is clamped to 16-bit range only. Generated code routes each APU write by owner (translate.py,
  `SND_ROUTINE_OWNER`).
- At every level load the sound-effect voice set is reset to power-on state
  (`Machine::reset_sfx_apu`, called from the first instruction of
  `Level_Initialize`'s real init body, `PRG008_A242` — see `LEVEL_INIT_BODY_ADDR`
  in translate.py). The sound engine re-arms only the music voices on a load, so
  without this a channel left ringing, mid-envelope or mid-DMC-sample by the
  previous level keeps playing on under the new level's music.
- Deliberate deviation from the ROM: the four music gates that skipped music notes
  while a sound effect was on the same channel are removed (`GATE_TARGETS` in
  translate.py). The translator reports them and checks each one.
- 48 kHz mono int16 output over SDL3; cycle-stamped register writes
- Output-rate nudge (±0.5%) holds the device queue at its target
- DMC fetches sample bytes from CPU memory

**Input**

- Keyboard and gamepad (SDL3), with hot-plug support
- Remappable via `settings.cfg`; unknown names fail fast with a clear message
- In-game menu: fullscreen toggle, FPS counter, widescreen toggle (pauses the game)

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
| Menu (fullscreen, FPS counter, widescreen) | F11 | L3 + R3 |
| Save session so far as `replay-N.rec`, then quit | G | — |
| Quit | Esc | — |

G writes every controller byte since the program started to the first free `replay-N.rec` in the
working directory; replay it with `smb3_headless --replay replay-N.rec` (add `--widescreen` if the
picture was widescreen when you pressed G, and note the mode is a startup setting for the replay).

F11 (or L3+R3 on a gamepad) opens a menu over the game and pauses it.
Up and Down move the cursor; Enter or X selects (D-pad and South on a
gamepad). East or Start closes the menu on a gamepad; Esc quits the program.
Menu items: Fullscreen, FPS counter, Widescreen.
On/off states show check/cross icons; the selected row gets a red-orange
arrow cursor. The menu is drawn with the game's own font tiles inside a
world-map status strip frame: white stroked text on tan with white trim.
Close the menu with F11 (L3+R3 on a gamepad).

Picture keeps square pixels and fits inside the window: it is scaled to fill
as much as possible, and any leftover area is black (pillarbox/letterbox).
Nothing is cropped by the window fit. The startup window is 720x720, exactly
3x the visible 240x240 4:3 picture (852x480, 2x the 426x240 widescreen
picture, with widescreen enabled).

### Widescreen (16:9)

The Widescreen menu item (`video.widescreen = 1`) widens the picture from 256
to 426 columns at the same pixel scale and the same 240 rows. The frame itself
is still drawn by the same per-dot renderer, byte for byte what the NES shows;
the added 85 columns on each side are composed after the frame, from game
state only, and the object window is widened so the game itself fills the
added columns; with widescreen off none of that runs and the picture and the
game are exactly the 4:3 ones.

- The frame is pasted once and never redrawn; only the columns outside it are
  composed here. The picture's middle is the frame with its overscan columns
  cropped (`screen x 0-9 and 246-255`, two columns further in than the NES
  blanks, so the half-tile smear at the frame's edge never shows), so wide mode
  does not show the NES's blanked edge columns; the two 10-column strips the
  crop removes are composed like any other extra column, so the crop leaves no
  hole, no duplicate and no stripe. Everything is aligned to the scroll the
  frame was drawn with (the first playfield row's captured `v` register), not to
  `Horz_Scroll`, which the NMI has already advanced for the next frame; that is
  what keeps it from slipping a pixel or two against the frame while the camera
  moves.
- The added columns show the level columns really there: the same `Tile_Mem`
  data, layout tables, CHR banks and palettes the game streams its own
  nametable from. Block changes (hit blocks, broken blocks, bridges) appear on
  both sides as you play, and no geometry is stretched, repeated or invented.
- Enemy and item sprites are drawn in the added columns at their real world
  position, with the game's own patterns and attributes. A drawer hides a piece
  outside the 256-pixel frame by skipping its Y-byte store and some drawers then
  read that missing value back (a troopa's head is drawn only if the body under
  it was), so hiding pieces cannot be undone from the frame's staging buffer
  alone. `wide_after_state_action` therefore runs once per object, from the
  object loop's own call site (see `WIDE_CALLS` in
  `tools/translate/translate.py`): an object that reaches beyond the picture is
  drawn a second time with the scrolls set so it sits at 100,100. The state
  action recomputes which of its sprites are off-screen before it draws, so that
  position - not just cleared visibility bits - is what makes its drawer write
  every piece: head, wings, feet, the mask a piranha plant hides behind, and the
  palette corrections it applies to them. It also draws into a scratch sprite
  region, because the game's own region rotates every frame and a plant shifts
  its own copy by 8, which is how a sprite could land past the end of OAM and
  vanish for a frame. Those pieces are then the only source used for that
  object: mixing them with the frame's culled records moved a sprite by a pixel
  whenever the frame happened to cull a different piece, which is what made
  sprites flicker at the picture's edge. The whole machine is rolled back
  afterwards, so the game never sees the second run; only the pieces stay. Each
  piece keeps the sprite slot it would have had in the game's own rotation,
  including a piece its drawer writes before its region (a troopa's head, the
  mask a plant hides behind), so pieces of one object stack exactly as the PPU
  stacks them - the mask stays in front of the plant on every frame. An object
  whose drawer did not run at all this frame (it was spawned or removed after
  the object loop) contributes no pieces: its region still holds the previous
  owner's bytes, and drawing those would blink a sprite of the old object into
  the new one's position for a single frame. The player keeps the frame's own Y
  records instead, because re-running its action would move it twice.
- Only horizontally scrolling levels are widened. The world map, title,
  ending, pipe transitions and vertical levels keep the 4:3 frame centred on
  black, as does a single frame in which the camera and the level data briefly
  describe different levels while a pipe reloads it.
- The window stops at the level's ends: the frame slides off-centre and the
  black beyond the level's last screen is never shown. The player stays
  centred while there is level on both sides and drifts toward the frame's
  edge at the ends, just as the camera does. Scripted camera moves (pipe
  transitions, the level-end walk, autoscroll pushes) move the frame as the
  game does; the wide window follows it rather than re-centring the player.
- Enemies in the added columns are real, live objects: widescreen widens the
  game's own object window, so the game creates, animates and draws them 96
  pixels further ahead than 4:3 does (the smallest 16-pixel column step that
  covers the 85 added columns). The game spawns an object when its look-ahead
  column (`Horz_Scroll + 272`) reaches it and deletes it once it is 288-384
  pixels ahead, or 48-128 pixels behind; the two instructions that add those
  table values are routed through `Machine::wide_spawn_lookahead` /
  `wide_delete_ahead` (see `WIDE_TUNE` in `tools/translate/translate.py`),
  which widen the spawn window by 96 pixels and the delete window by 176
  while widescreen is on. The delete window needs the larger figure: an
  enemy walking off the left of the 4:3 frame would otherwise be deleted
  while it is still visible in the added columns, and an enemy created at the
  spawn window would be deleted only sixteen pixels later, so it could never
  walk in at all. Both hooks return their input unchanged in 4:3, so the
  4:3 game stays bit-for-bit the same.
  The spawner creates only the objects sitting exactly on its look-ahead
  column, so a column it is never shown is an object that is never created -
  and never comes back. The window therefore walks out to the widened column
  one 16-pixel column per call instead of jumping there, and it never presents
  a column the picture already shows, because an object created there is seen
  to pop in. Two moments are exempt, both under a level's start card: while
  the game builds the level (its level loader sweeps the camera over the first
  screen and needs the game's own column to create the level's opening
  objects), and the rightward window's walk-out from that same column to the
  widened one at the level's first frames. Everywhere else the window follows
  the widened column, so an enemy walks in from the picture's edge instead of
  appearing inside it. The window resets on `Level_Initialize` only
  (`Machine::wide_level_load`); when the mode is switched on mid-level it
  starts at the widened column, because everything the 4:3 window reaches
  already exists. This is the mode's only
  gameplay difference: enemies activate, and start moving toward the player,
  earlier than they would with 4:3 framing. The widened window applies to the
  game's whole object table, so scripted objects on the same screen (Toad, an
  autoscroll trigger, a treasure setting) also fire up to 96 pixels earlier,
  just as they would if the television were wider.
- The hardware's 8-sprites-per-line limit and the sprite-0 hit apply to the
  frame the game drew; a line in the added columns can show more sprites than
  a real PPU would. Screen-space spare sprites (score popups and the like)
  have no world position and are not repeated outside the frame.
- The status bar is pasted at the centre of the picture, so the HUD stays
  centred even at a level end, when the frame itself has slid toward the edge.
  The paste starts at the bar layer's first row, not at the split IRQ, so the
  level rows between the two stay aligned with the playfield they belong to
  instead of sliding with the bar. In the added columns the playfield continues
  downward beside the bar, drawn with the playfield's own palette and banking,
  so the ground runs on instead of the bar's HUD tiles. The bar is
  background-only, so no HUD sprite needs to move.
- The last screen of a level is mostly empty backdrop in the level data itself
  (the area leading into the ending sequence). The added columns show that
  backdrop, the same black the game streams into its nametable there, rather
  than invented scenery.

The wide columns were verified against the game's own output: at a freshly
loaded level every streamed column's pattern and palette from the PPU's
nametable and scrolling matches the composed ones bit for bit, the middle of
every wide picture is the 4:3 framebuffer's own cropped columns, and an enemy
in the added columns renders with the pieces and the palette its own drawer
gives it (every piece the second run produces paints, and over a walk all but a
handful of the painted sprite pixels are one of the four colours of that
piece's palette, the rest being higher-priority sprites on top). The second run
is invisible to the game: a 2600-frame run ends with the same PC, cycle count,
camera, object table and timers with it enabled or disabled. The object window
is the game's own: with widescreen off every object still spawns at a lead of
272 pixels and with it on at 368, so gameplay on the 4:3 side is untouched.

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
| `video.widescreen` | `0` | 16:9 wide picture instead of 4:3 (`0`/`1`) |
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
./build/smb3_headless --assets assets --frames 2400 --widescreen \
    --script "150:8,156:0,600:8,606:0,1500:128" --shots 700,1600,2250 --shot-dir shots
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--assets DIR` | `assets` | Asset pack with `prg.bin` (256 KB) + `chr.bin` (128 KB) |
| `--frames N` | `300` | Frames to run with no window |
| `--press-start N` | off | Hold Start for 6 frames starting at frame N (`0x08`) |
| `--script "F:M,..."` | off | Controller mask M (bit 0 = A, 1 = B, 2 = Select, 3 = Start, 4 = Up, 5 = Down, 6 = Left, 7 = Right) held from frame F until the next entry |
| `--out FILE` | `frame.png` | Final frame as PNG (stored-deflate, no zlib) |
| `--widescreen` | off | Output the 426x240 wide picture instead of the 256x240 frame |
| `--shots N,...` | off | Write that many-frames-in picture to `DIR/fNNNNN.png` |
| `--shot-dir DIR` | `.` | Directory for `--shots` |
| `--wav FILE` | off | Full-run mono int16 WAV at the APU output rate |
| `--dump-state` | off | Print level, scroll, split and per-scanline rows after the run |
| `--dump-level` | off | Print the loaded level (`Tile_Mem`) as two hex digits per block |
| `--trace N` | off | Print the state line every N frames |
| `--poke "F1[-F2]:ADDR=VAL,..."` | off | Force RAM bytes over a frame range (diagnostic: holds the player alive, scripts camera sweeps) |

The runner prints the frame count, output path, and CPU state
(`pc`, `cyc`). APU writes are applied and advanced inside `render()` every
frame, exactly as in the desktop app.


---

## Layout

- `src/runtime/` — NES runtime (CPU helpers, memory map, MMC3, PPU, APU, palette,
  `hud.cpp` status-bar layer, `wide.cpp` widescreen compose)
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
