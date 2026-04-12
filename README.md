## DoomWorks (fork of GBADoom by doomhack)

Originally a port of prBoom to the GBA, then ported to the Numworks calculator.

Works on both simulator **AND REAL DEVICE** (only tested on N0110) !

https://github.com/user-attachments/assets/15cdd241-f8f5-4c12-888f-607869b79a1a

This project uses [GbaWadUtil](https://github.com/doomhack/GbaWadUtil) (by doomhack) from the original GBADoom repo to embed the chosen WAD into the app.

## IMPORTANT:

**This port is still very much a work in progress / proof of concept !!!**

The biggest challenge when porting this was RAM usage, as a Numworks app only has about **~100KB** of heap available, which is almost nothing when compared to the original PC recommandations (~4MB) and still less than half of the GBA's specs (~256KB).

The available storage space for a Numworks app was also a major constaint, as on my 25.2.0 Epsilon N0110 device, I could only install an app of about 2.4MB total.

As a result, most WADs (including the basic shareware WAD) either won't fit when installing the app on a regular stock Numworks device, or will take too much memory from the Zone allocator and quit.

### RAM / Allocation issues:

Instead of using DOOM's builtin Zone allocator, this project uses the system allocator (malloc / free). The performance cost is negligible.

However, the calculator only give around 100KB of memory for the entire app. This means that most maps will not load. I am still working on optimizing ram usage for the future.

From what I have heard, N0120 calculator have double the ram compared to all other models. I don't know if that extra ram is available for apps as I don't have this model, but if it is, most maps of WADs using the "compact" format should work without issue.

### WAD size issues:

If your WAD is too big to fit on your device, here are some projects that can help you port your favorite WADs :

- [Wadptr](https://github.com/fragglet/wadptr) : A Doom WAD file compressor, almost necessary to port WADs to this project. (by fragglet)
- [Miniwad](https://github.com/fragglet/miniwad) : A (very) minimalist Doom IWAD, letting you use Wadptr to its full potential. (also by fragglet)

**Miniwad + Wadptr = A tiny WAD that can (probably) fit into your Numworks.**

In case you need even more space, you can use [Nwagra](https://yaya-cout.github.io/Nwagyu/guide/help/enlarge-your-memory.html) (by yaya-cout), a tool letting you use up to 6MB to install apps (for N0110, N0115 and N0120) !

If you just want a small WAD to test out the engine, I recommand [Squashware Doom](https://github.com/fragglet/squashware) (by fragglet). The silent 1-level version of this fits perfectly and runs at full-speed.

## To do:

- Make a better zone allocation system (utilize the extra RAM of the N0120, use dynamic allocations)
- Fork [GbaWadUtil](https://github.com/doomhack/GbaWadUtil) to make an easier to use version that strips the music + SFX from the WADs (for now, Numworks calculators don't have speakers, but maybe in the future !)
- Optimize as much as possible !

## Controls:

- UP, DOWN, LEFT, RIGHT : D-Pad
- A (Use, Sprint, Menu) : OK
- B (Shoot, Back in menus) : Back
- L (Strafe left) : Shift
- R (Strafe right) : Alpha
- Start (Open menu) : EXE
- Select (Automap) : Toolbox

Changing weapon is done with (Shift / Alpha) + OK.

## Cheats (from the original GBADoom):
**Chainsaw:** L, UP, UP, LEFT, L, SELECT, SELECT, UP  
**God mode:** UP, UP, DOWN, DOWN, LEFT, LEFT, RIGHT, RIGHT  
**Ammo & Keys:** L, LEFT, R, RIGHT, SELECT,UP, SELECT, UP  
**Ammo:** R, R, SELECT,R, SELECT,UP, UP, LEFT  
**No Clipping:** UP, DOWN, LEFT, RIGHT, UP, DOWN, LEFT, RIGHT  
**Invincibility:** A, B, L, R, L, R, SELECT, SELECT  
**Berserk:** B, B, R, UP, A, A, R, B  
**Invisibility:** A, A, SELECT,B, A, SELECT, L, B  
**Auto-map:** L, SELECT,R, B, A, R, L, UP  
**Lite-Amp Goggles:** DOWN,LEFT, R, LEFT, R, L, L, SELECT  
**Exit Level:** LEFT,R, LEFT, L, B, LEFT, RIGHT, A  
**Enemy Rockets (Goldeneye):** A, B, L, R, R, L, B, A  
**Toggle FPS counter:** A, B, L, UP, DOWN, B, LEFT, LEFT  

## Building and running (current repo workflow):

This repository uses the top-level `Makefile` to build and run the NumWorks app.  
[GbaWadUtil](https://github.com/doomhack/GbaWadUtil) (used to embed WADs into the project to save on RAM) is already wired in and is called automatically when embedding a WAD.

### Requirements

- `make`
- A working C/C++ build environment for the NumWorks app toolchain used by `numworks_app/`
- `epsilon.bin` (Linux) or `epsilon.app` (macOS) in the repository root for simulator runs
- `npx` (for `nwlink`) if you want to install on a real device

### Run on a NumWorks device

Embed the WAD in the app :

```bash
make PLATFORM=device run WAD=doom1.wad GBADOOM_ENABLE_STACK_REUSE=1 USE_UNSTABLE_ZONE_HEAP_SIZE=1 USE_EXTERNAL_IWAD=0 -j4
```

Notes:

- Embedded WAD source files are generated under `source/iwad/` automatically.

### Run on simulator

Build and run with a WAD:

```bash
make PLATFORM=simulator run WAD=doom1.wad -j4
```

Build only:

```bash
make PLATFORM=simulator build WAD=doom1.wad -j4
```

Clean:

```bash
make clean
```

## For anyone reading the code :
### WAD format terms used in this repo (non-official terminology)

The terms **vanilla** and **compact** are local project terms, not official Doom formats.

- **Vanilla WAD**: standard Doom map lumps as stored in IWAD / PWAD files (classic vertex / linedef / sidedef / seg layouts, sidedef textures as 8-char names).
- **Compact WAD**: a WAD processed by [GbaWadUtil](https://github.com/doomhack/GbaWadUtil) where map lumps are preconverted for this engine (for example, sidedef textures become texture indices, and map geometry lumps are expanded into runtime-oriented layouts).

Both represent the same map content. Compact format is a preprocessing / packing step for GBADoom and this port.
