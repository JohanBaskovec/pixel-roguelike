# pixel-roguelike

A 2D side-view game where every pixel is simulated: gases, liquids and solids, each with its own
temperature, density and behaviour. It draws on *Oxygen Not Included* and *Noita*.

**Status:** an action roguelike in the making. Done so far (milestones M1 to M6a): a
multithreaded simulation of a 4096 × 2048 pixel world (one pixel is 5 cm), water with pressure,
heat with melting, freezing and boiling, gases as mixtures with pressure (air, CO2, hydrogen,
water vapour) that diffuse, rise, sink, condense and burn, powders and digging, lava and molten
metals, creatures that breathe, and a player (100 hp, health crates to pick up) who runs, jumps, flies with a
jetpack and fires a gun whose bullets, which the player draws pixel by pixel (solids,
liquids, gases, heat), and casings are real pixels, against monsters that
chase and bite and bleed when shot. The sandbox tools are still there: you can paint any of it, heat and
cool it, and watch. Not done: ranged enemies, world generation, saving. Busy scenes (long
bursts of fire, big gas clouds) can drop below 60 ticks per second; the plan to fix that is in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md). See [docs/ROADMAP.md](docs/ROADMAP.md).

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
| `A` / `D`, `W` | Run left / right, jump; hold `W` to fly with the jetpack (the physical keys: `Q` / `D`, `Z` on AZERTY) |
| Left mouse | Rifle tool (`E`, the default): fire, aimed at the cursor (hold for automatic fire). Other tools: paint the selected element (hold still to keep pouring) |
| Right mouse | Erase (paint void), including the box walls |
| Shift + left mouse | Paint only into empty (void) cells |
| Mouse wheel, `[` / `]` | Brush radius 0–256 px (0 = one pixel) |
| `-` / `=` | Gas and Air brushes: paint at half / twice the pressure (1 atm to start; limited to what the gas can hold before it would be liquid: steam its saturation pressure, the others the density of their liquid, e.g. about 860 atm of hydrogen) |
| `B` | Toggle circle / square brush |
| `-` / `=` | Gas and Air brushes: paint at half / twice the pressure (1 atm to start, shown in the panel), up to what the gas can hold before it would turn liquid: steam its saturation pressure, the others the density of their liquid (about 860 atm of hydrogen) |
| Middle mouse drag, arrows | Pan the view (stops following the player) |
| `F` | Follow the player again |
| `Ctrl` + mouse wheel | Zoom in / out around the cursor (1/4× to 8×; starts at 2×) |
| `Home` | Show the whole world |
| `P` | Pressure view: water coloured by depth below its surface, gas by pressure (log scale, green = 1 atm) |
| `T` | Temperature view |
| `1`–`9`, `0` or click the toolbar | Water, Oxygen, CO2, Hydrogen, Water vapour, Dirt, Rock, Iron, Ice, Eraser |
| `N` / `G` | Nitrogen / Air (every pixel the same mixture: 21% oxygen, 0.04% CO2, the rest nitrogen), at 1 atm |
| `K` | Put a wandering test creature at the cursor (it breathes the air around it) |
| `I` | A log (a rigid body: it falls, tips and rolls, and a bullet cuts it in two) at the cursor |
| `J` / `L` / `O` / `V` | Sand / Lava (1600 K) / Oil / Wood (burns from 250 °C, needs 16% oxygen, gives CO₂ and water: light it with the Heat brush or lava) |
| `X` | Dig: loosens dirt into loose soil and rock into rubble, which fall |
| `H` / `C` | Heat / Cool brush: hold to raise or lower temperatures (8 K per frame). Heat lights hydrogen mixed with oxygen (773 K) |
| `Space` | Pause / resume |
| `.` | Advance one tick (pauses) |
| `Tab` | The gun menu: the gun's slots (1 to 10 bullets, fired in turn, then a reload), each with its design shown and an Edit button: a window like Paint to draw a bullet (solids, liquids, gases with a density, heat; released on impact or gradually). The game waits while it is open |
| `R` | Reload the gun now (it reloads by itself after the last slot) |
| `F5` | Restart: a new level |
| `Esc` | Quit |

The panel shows FPS, simulation time per tick, drawing time, frame time, threads, how many cells
were updated, water moved by pressure, gas pressure equalisations, free particles in flight
("Flying"), chunks conducting heat, active chunks, zoom, and the element, temperature (with
melting/boiling progress), (for water) depth and absolute pressure (gas on the surface plus
depth), (for gas) pressure and mass under the cursor, plus the partial pressure of each gas in a
15 × 15 window around it. Below, the player's state, health and shots, and the first creatures
with their health and the O₂ and CO₂ around their heads. The game starts in a maze
carved in dirt with boulders, with the player standing in a room near the middle. The demo scene
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
