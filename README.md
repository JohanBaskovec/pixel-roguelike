# pixel-roguelike

A 2D side-view game where every pixel is simulated: gases, liquids and solids, each with its own
temperature, density and behaviour. It draws on *Oxygen Not Included* and *Noita*.

**Status:** milestone M1 (sandbox box) is done. You can paint water, gases (oxygen, CO2,
hydrogen) and solids (dirt, rock, iron, ice) into a box with dirt walls and watch them behave.
See [docs/ROADMAP.md](docs/ROADMAP.md).

## Tech

C17 · SDL3 · CMake. A chunked cellular-automaton simulation running on the CPU (single-threaded
in M1; multithreading arrives in M2).

## Build

You need a C17 compiler (GCC, Clang or MSVC) and CMake ≥ 3.24. SDL3 is downloaded and built
automatically at configure time.

On Linux, SDL needs these development packages (Debian/Ubuntu names):

```bash
sudo apt-get install ninja-build libx11-dev libxext-dev libxcursor-dev libxrandr-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev libdecor-0-dev
```

Then:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/release -j
```

```bash
./build/release/pixel-roguelike
```

Options: `--width N --height N` (world size, rounded up to multiples of 64; default 1024×768),
`--seed N`, `--demo` (start with a demo scene).

Tests and benchmark:

```bash
ctest --test-dir build/release --output-on-failure
```

```bash
./build/release/pr-bench
```

## Controls

| Input | Action |
|---|---|
| Left mouse | Paint the selected element (hold still to keep pouring) |
| Right mouse | Erase (paint void), including the box walls |
| Shift + left mouse | Paint only into empty (void) cells |
| Mouse wheel, `[` / `]` | Brush radius 0–256 px (0 = one pixel) |
| `B` | Toggle circle / square brush |
| `1`–`9` or click the toolbar | Water, Oxygen, CO2, Hydrogen, Dirt, Rock, Iron, Ice, Eraser |
| `Space` | Pause / resume |
| `.` | Advance one tick (pauses) |
| `R` | Reset to the empty box |
| `Esc` | Quit |

The panel shows FPS, simulation time per tick, how many cells were updated, active chunks, and
the element and temperature under the cursor.

## Documentation

- [CLAUDE.md](CLAUDE.md): start here (contributor and agent guide)
- [docs/VISION.md](docs/VISION.md): what the game is trying to be
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the code is structured
- [docs/SIMULATION.md](docs/SIMULATION.md): how the physics works
- [docs/ELEMENTS.md](docs/ELEMENTS.md): element reference values
- [docs/PERFORMANCE.md](docs/PERFORMANCE.md): budgets, techniques, benchmark log
- [docs/CODE_STYLE.md](docs/CODE_STYLE.md): C conventions
- [docs/DECISIONS.md](docs/DECISIONS.md): decision log
