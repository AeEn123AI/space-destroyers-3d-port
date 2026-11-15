# Space Destroyers 3D: PC + Nintendo 3DS port

A port of the Scratch game *Space Destroyers 3D* by AeEn123, made from the original `.sb3`.
It builds for **PC (SDL2 + OpenGL 2.1)** and for the **Nintendo 3DS** (`.3dsx` and `.cia`, with real per-object
stereoscopic 3D). Both builds run the same game code in `src/common`. That code reproduces the Scratch scripts'
timing on a 30 Hz "Scratch frame" grid, while motion is smooth at any frame rate.

## Layout
```
src/common/      game logic (game.c), software audio mixer (audio.c), PNG + mip loader (image.c), font baking (font.c)
src/pc/          SDL2/OpenGL backend
src/3ds/         citro2d/citro3d + ndsp backend (stereo top screen, touch/HUD bottom screen)
tools/           build_assets.py (asset pipeline), azahar.sh + shot.py (headless emulator testing), fonts/, bin/ (makerom, bannertool, ctrtool)
cia/app.rsf      makerom spec (title ID 000400000C5D3D00)
third_party/     minimp3, dr_wav, stb_image, stb_image_write, stb_truetype, stb_rect_pack (public domain / MIT)
NOTES.md         the Scratch behaviour this port reproduces (timings, quirks, layouts)
```

## Building
**The game's assets are not included in this repo.** The music is Sega's (SMS BIOS / Space Harrier), so it can't be
redistributed here. To build, download the original project from Scratch (open the project in the Scratch editor,
then File > Save to your computer) and put it in the repo root as `Space Destroyers 3D.sb3`. The asset pipeline below
extracts everything from it.

Requirements: Python 3 with numpy + Pillow, `rsvg-convert` (librsvg2-bin), `ffmpeg`, a C compiler, SDL2 dev files and
OpenGL. The 3DS build also needs devkitPro (devkitARM, libctru, citro2d, citro3d).

```sh
make assets          # sb3 -> build/assets/{pc,3ds,cia} + src/common/costumes_gen.h  (SVGs rendered with rsvg)
make pc              # build/pc/space-destroyers-3d + build/pc/assets/
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
make 3ds             # space-destroyers-3d.3dsx
make cia             # space-destroyers-3d.cia (uses tools/bin/makerom + bannertool)
make dist            # dist/: .cia, .3dsx, space-destroyers-3d-linux.zip
```
The asset pipeline regenerates everything from `Space Destroyers 3D.sb3`:
- The SVG costumes are rendered at high resolution for PC (enough for 4K at the largest sizes Scratch allows) and at
  the exact size needed for the 3DS top screen (power-of-two, RGBA8).
- Transparent pixels get colour bled into them (push-pull), so there are no dark fringes. Mipmaps are built at load
  time with an alpha-weighted box filter.
- The music and sound files are copied **unmodified** (44.1 kHz / 22.05 kHz MP3s and the original WAVs) and decoded
  at runtime.
- It also makes the 3DS Home Menu banner (from the Thumbnail art), the 48×48 icon, and the banner jingle (the first
  2.9 s of the Intro, faded out).

Windows: the code is plain C99 + SDL2 + OpenGL loaded through `SDL_GL_GetProcAddress`. A MinGW build should only
need `-lmingw32 -lSDL2main -lSDL2 -lopengl32` in place of `-lGL`. It has not been tested.

## Controls
| Action | PC keyboard / mouse | PC gamepad | 3DS |
|---|---|---|---|
| Move | WASD / arrow keys | left stick / D-pad | Circle Pad / D-Pad |
| Steer like Scratch's mobile mode | click-drag (tap = shoot) | | drag on the touch screen (tap = shoot) |
| Shoot | Space | A | A |
| Speed up / down | E / Q | RB, RT / LB, LT | R, ZR / L, ZL |
| Pause | Enter or P, or click Pause | Start | START or touch Pause |
| Back to title | Esc | Back | SELECT (on the title: exit) |
| Title menu | mouse hover + click, arrows + Enter, Space = Play | D-pad + A, B = back | D-Pad + A, B = back, START = play, or touch |
| Toggle music | 1 or the Toggle Music button | Y | X or the Toggle Music button |
| 3D colour shimmer (off by default) | | | touch the switch on the title or pause screen |
| Fullscreen | F11 | | |

3D colour shimmer is a 3DS-only option. With the 3D slider up, your bullets and the ammo pellets are drawn in
opposite colours for each eye (they keep their normal colour cycling), which makes them shimmer. With the slider
down they look normal. The setting is saved with the records.

The original's secrets are still there: Up, Down, Left, Right, B, A (on the 3DS that's the D-Pad, then B, A) and
Q W E R T Y.

## Notes on faithfulness
- The objects and spawners run as Scratch threads would, in the same order on a 30 Hz tick. That keeps the
  original's shared-costume quirk: the star spawner picks up the costume that the power-up, planet or bullet thread
  last switched to. In the original this is why power-ups and planets come in bunches.
- One deliberate difference: Scratch keeps sprites fenced inside the stage and caps their size, so the original's
  planets stuck to the screen edges. The port doesn't copy that. Stars and planets fly off the edges and keep growing
  as they get close.
- The two music mixes (Intro→Main and Options intro→Options select) are decoded in lockstep and crossfaded like the
  original's two sprite volumes, so they can never drift apart.
- The world records start at the project's cloud values (687.19 s / 1348). Local bests are saved:
  - PC: `~/.local/share/AeEn123/Space Destroyers 3D/records.txt`
  - 3DS: `sdmc:/3ds/space-destroyers-3d/records.txt`
- Multiplayer relied on Scratch cloud variables, so the Toggle Multiplayer button is left out.
- On real 3DS hardware, audio needs `sdmc:/3ds/dspfirm.cdc` (dump it with DSP1). Without it the game runs silently.

## Headless testing
- PC: set `SD3D_SEED`, `SD3D_FIXED_DT` and `SD3D_SCRIPT`, for example:
  `"0:move=60,-10;4:shot=title.png;4.5:down=space;4.7:up=space;10:shot=game.png;11:quit"`.
  Actions are `down`/`up`=<SDL key name>, `move`=x,y (stage coords), `mdown`, `mup`, `shot`=file, `size`=WxH and
  `quit`. `SD3D_ASSETS` overrides the asset directory.
- 3DS: `tools/azahar.sh start|stop|key|shot` drives Azahar on an Xvfb display.

## Credits and licences
The game and art are by AeEn123. The music is Sega's (Sega Master System BIOS music / Space Harrier), credited in the game,
and is not included here.
Fonts: Grand9K Pixel (see `tools/fonts/Grand9K-Pixel-LICENSE.txt`), Liberation Sans (SIL OFL) and DejaVu Sans
(Bitstream Vera licence). Their licence files are in `tools/fonts/` and in the Linux zip.

## Screenshots
See [`screenshots/`](screenshots). There are no prebuilt downloads, since builds would contain the music.

There's also a separate Nintendo DS / DS Lite port: [space-destroyers-3d-ds](https://github.com/AeEn123AI/space-destroyers-3d-ds).
