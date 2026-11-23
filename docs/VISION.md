# Vision

## One-liner

A side-view world where every pixel is real matter. Gases drift and stratify, water finds its
level, heat flows through rock and metal, and the player reshapes all of it with a pixel-precise
brush.

## Genre

**An action roguelike** (decided 2026-11-23, D-031). The player is a character in the world, 1.85 m
tall, with weapons (an automatic rifle first) that act on the simulation like everything else: a
bullet is a pixel of lead that flies, digs and falls. We only ever see part of the level around
the player. The sandbox tools stay available for now.

## Inspirations: what we take, what we leave

| From | We take | We leave (for now) |
|---|---|---|
| *Oxygen Not Included* | Gases you must manage (O2, CO2, H2); temperature as a core system; heavy gases sinking and light ones rising; digging orders instead of direct control; colony-scale thinking | The tile grid. ONI simulates 1 m tiles; we simulate pixels |
| *Noita* | Every pixel simulated; falling-sand liquids and powders; fully destructible terrain; emergent chaos; chunked multithreaded simulation | (Undecided) Its roguelike structure and wand-crafting |

## Pillars

1. **The world is the simulation.** Nothing is decor. If you can see it, it is a cell, and it
   behaves like what it is.
2. **Believable physics.** We start from real-world values (density, heat capacity, conductivity,
   phase-change temperatures) and only bend them on purpose, for example to speed up heat flow so
   it happens at a playable pace.
3. **Pixel-precise agency.** All interaction goes through a brush that can be as small as one
   pixel or as large as you like. There are no tiles and no snapping.
4. **Scale.** Millions of cells, simulated at 60 ticks/s. Performance is part of the design, not a
   polish step.

## Controls philosophy

Keyboard and mouse. The mouse paints and selects; the wheel resizes the brush. The keyboard
switches tools and elements and controls time (pause, step, speed).

## Look

- Each element has a base colour plus per-pixel variation (`shade`), so materials read as
  textured rather than flat.
- Gases are invisible in reality, but here they are drawn as faint tints so you can read them, as
  in ONI. This is a deliberate deviation from realism.
- Later: hot cells glow (blackbody colour), a temperature overlay view, simple lighting.

## Decided physics choices

- Vacuum is a perfect insulator; there is no heat radiation (D-017).
- Water pressure uses a pressure field while keeping one element per pixel (D-018, milestone M2b).
- Creatures breathe at real rates by default (≈250 mL/min of O₂ at rest), with an optional
  gameplay speed-up (roadmap M4c).

## Open design questions

These are intentionally unresolved. Record answers in `DECISIONS.md` when they are made.

- **Genre.** Is this a colony sim (ONI), a roguelike with a player character (Noita, and the repo
  name), or a hybrid?
- **Agents.** Are there colonists who carry out orders, a single avatar, or both? (The creature
  engine in roadmap M4c — bodies that push gas and water around, breathing at real rates — is
  needed either way.)
- **Physical scale.** How big is one pixel? The working assumption is 5 cm (see `SIMULATION.md`).
  The answer affects character size, gas masses and heat time scales.
- **Gas model.** Gas can be one particle per pixel (Noita) or carry a mass per pixel with pressure
  (ONI). The plan is to start with particles and move to mass and pressure in M4; see
  `DECISIONS.md` D-005.
- **World.** Is the world one fixed-size map or streamed and procedurally generated? Do worlds
  persist?
- **Progression.** What does the player want, and what kills them or the colony?
