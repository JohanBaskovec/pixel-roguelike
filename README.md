# pixel-roguelike

A 2D side-view game where every pixel is simulated: gases, liquids and solids, each with its own
temperature, density and behaviour. It draws on *Oxygen Not Included* and *Noita*.

**Status:** milestones M1 (sandbox box), M2 (multithreading, big world, camera), M2b (water
pressure) and M3 (temperature) are done. You can paint water, steam, gases (oxygen, CO2,
hydrogen) and solids (dirt, rock, iron, ice) into a 4096 × 2048 box with dirt walls, heat and
cool them, and watch them behave: water levels out, fills communicating vessels, convects, freezes
and boils; hot air rises; hot iron glows.
See [docs/ROADMAP.md](docs/ROADMAP.md).

## Tech

C17 · SDL3 · CMake. A chunked cellular-automaton simulation running on all CPU cores.

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

Options: `--width N --height N` (world size, rounded up to multiples of 64; default 4096×2048),
`--threads N` (default: one per CPU core), `--seed N`, `--demo` (start with a demo scene).

Tests and benchmark:

```bash
ctest --test-dir build/release -j4 --output-on-failure   # each test on its own, 4 at a time
./build/release/pr-tests sand   # only the tests whose name contains "sand" (with timings)
```

```bash
./build/release/pr-bench
```

## Controls

| Input | Action |
|---|---|
| `A` / `D`, `W` | Run left / right, jump (the physical keys: `Q` / `D`, `Z` on AZERTY) |
| Left mouse | Rifle tool (`E`, the default): fire, aimed at the cursor (hold for automatic fire). Other tools: paint the selected element (hold still to keep pouring) |
| Right mouse | Erase (paint void), including the box walls |
| Shift + left mouse | Paint only into empty (void) cells |
| Mouse wheel, `[` / `]` | Brush radius 0–256 px (0 = one pixel) |
| `B` | Toggle circle / square brush |
| Middle mouse drag, arrows | Pan the view (stops following the player) |
| `F` | Follow the player again |
| `Ctrl` + mouse wheel | Zoom in / out around the cursor (1/4× to 8×; starts at 1×) |
| `Home` | Show the whole world |
| `P` | Pressure view: water coloured by depth below its surface, gas by pressure (log scale, green = 1 atm) |
| `T` | Temperature view |
| `1`–`9`, `0` or click the toolbar | Water, Oxygen, CO2, Hydrogen, Water vapour, Dirt, Rock, Iron, Ice, Eraser |
| `N` / `G` | Nitrogen / Air (21% oxygen cells, evenly spread among nitrogen), at 1 atm |
| `K` | Put a wandering test creature at the cursor (it breathes the air around it) |
| `J` / `L` / `O` | Sand / Lava (1600 K) / Oil |
| `X` | Dig: loosens dirt into loose soil and rock into rubble, which fall |
| `H` / `C` | Heat / Cool brush: hold to raise or lower temperatures (8 K per frame). Heat lights hydrogen mixed with oxygen (773 K) |
| `Space` | Pause / resume |
| `.` | Advance one tick (pauses) |
| `R` | Reset the level |
| `Esc` | Quit |

The panel shows FPS, simulation time per tick, drawing time, frame time, threads, how many cells
were updated, water moved by pressure, gas pressure equalisations, free particles in flight
("Flying"), chunks conducting heat, active chunks, zoom, and the element, temperature (with
melting/boiling progress), (for water) depth and absolute pressure (gas on the surface plus
depth), (for gas) pressure and mass under the cursor, plus the partial pressure of each gas in a
15 × 15 window around it. Below, the player's state, health and shots, and the first creatures
with their health and the O₂ and CO₂ around their heads. The game starts in a level of hilly dirt
with rock strata and boulders under air, with the player standing in the middle. The demo scene
(`--demo`) has a tall tank with a hole near its foot, which jets, and a room of air with a
wandering creature (and the player).

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
