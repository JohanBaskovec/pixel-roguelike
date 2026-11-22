# pixel-roguelike

A 2D side-view game where every pixel is simulated: gases, liquids and solids, each with its own
temperature, density and behaviour. It draws on *Oxygen Not Included* and *Noita*.

**Status:** pre-code. The design and engineering docs are written. The first milestone (a sandbox
box where you paint gas, walls and water) is next. See [docs/ROADMAP.md](docs/ROADMAP.md).

## Tech

C17 · SDL3 · CMake. A multithreaded, chunked cellular-automaton simulation running on the CPU.

## Build (once M1 lands)

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/release -j
```

```bash
./build/release/pixel-roguelike
```

SDL3 is downloaded automatically at configure time. You need a C17 compiler (GCC, Clang or MSVC)
and CMake ≥ 3.24.

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
