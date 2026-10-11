# SW3D

Star Wars (Atari, 1983) on macOS/Linux with the depth put back: the game's own
3D points, recovered from its math hardware as it runs, so the picture can be
drawn from a different eye position (and, later, once per eye in a headset).

It is built on the C port in
[tcottrill/Star-Wars-Full-Disassembly-and-C-Port](https://github.com/tcottrill/Star-Wars-Full-Disassembly-and-C-Port)
(commit c4b3d15, GPL-2.0-or-later, see LICENSE): `src/` is that project's
`c_src/`, without its Windows backend, oracle and tests. There is no 6809 and
no emulator in it. No ROM data is included.

## Build and run

    brew install sdl2                 # macOS; any SDL2 development package on Linux
    ./build.sh
    ./sw3d

The ROM images go in `roms/` (six `.bin` files). Make them from your own MAME
`starwars` set with the upstream project's `python disasm/image.py your.zip`
and copy `disasm/build/*.bin` here. `roms/` is in `.gitignore`.

## Keys

| | |
|---|---|
| flight yoke | mouse (F8 releases / captures it), or arrow keys |
| fire | left mouse button, Left Ctrl or 1; second trigger: right button or Left Alt |
| coin, start | 5 (6, 7 the other coin switches); then fire |
| **F6** | next view: 2D original, 3D moving eye, 3D side by side, 3D red/cyan glasses |
| **F7** | colour the lines by depth (near red, far blue, the flat panel grey) |
| F5 | 3D moving eye: stop / restart the sway. I J K L move the eye, U O back and forward |
| [ ] | eye separation for the two stereo views |
| - = | distance of the flat panel (scores, cockpit, sights) |
| F11, Esc | fullscreen, quit |
| F2 F1 F3 F4 | self-test switch, diagnostic, slam, sound board test |

## What was added to the C port

| file | |
|---|---|
| `src/platform/sdl/plat_sdl.cpp` | the SDL2 + OpenGL backend and `main()`, and the 3D views |
| `src/sw3d_capture.c`, `.h` | records every perspective step the game asks its math hardware for; gives a picture point its depth |
| `src/fiber_posix.c`, `.h` | the Win32 fiber calls `port.cpp` uses, on ucontext |
| `src/port.cpp` | includes `fiber_posix.h` when not on Windows |
| `src/hw.cpp` | calls the capture around every write to the math registers ($4700-$4707) |
| `src/app.cpp` | `/` instead of `\` in the ROM image path |
| `src/gen/tcmes.c` | one generated line: `0x4E-0x5C` needs spaces for compilers other than MSVC (the fix belongs in upstream's `tools/xlate.py`) |

## How the depth is recovered

The game projects a point by dividing by its depth X (the hardware divider,
$4704) and running a matrix-processor program that multiplies Y and Z by the
quotient: `MS_PERS` ($86) for most things, `MS_PRS2` ($AE) for ground objects.
`sw3d_capture.c` notes the result of each one together with the X the divider
held. A point's place in the picture is then exactly

    picture x = 125 + (255/1024) * 512 * Y / X
    picture y = 165.8984 - (255/1024) * 512 * Z / X

so every line end the vector generator draws can be looked up (within 0.3 of
a picture unit, among the points of the last few pictures) and given its X.
Lines with a depth at neither end are the 2D parts of the picture - scores,
messages, the cockpit and sights - and sit on a flat panel.

Known limits: a line clipped at the screen edge has a depth at one end only
and borrows it for the other, so it is slightly wrong in 3D; lines are matched
by position, so a 2D line end that happens to land on a 3D point picks up its
depth; nothing has been done yet about the cockpit, which should be its own
near layer in a headset.
