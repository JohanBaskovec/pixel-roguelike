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
| *Oxygen Not Included* | Gases you must manage (O2, CO2, H2); temperature as a core system; heavy gases sinking and light ones rising | The tile grid (ONI simulates 1 m tiles; we simulate pixels). Since the game became an action roguelike (D-031): digging orders instead of direct control, colony-scale thinking. Separate elements for mixtures ("water" and "dirty water"): here gases mix in a cell (D-035), and liquids should too (roadmap M6c) |
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
- Hot cells glow (blackbody colour), and there is a temperature overlay view. Later: simple
  lighting.

## Decided physics choices

- Vacuum is a perfect insulator; there is no heat radiation (D-017).
- Water pressure comes from labelled bodies of still liquid, one element per pixel (D-018,
  milestone M2b).
- A gas pixel holds a mixture of gases; air is one such mixture (D-035).
- One pixel is 5 cm (D-037).
- Creatures breathe at real rates by default (≈250 mL/min of O₂ at rest), with an optional
  gameplay speed-up (roadmap M4c).

## Open design questions

These are intentionally unresolved. Record answers in `DECISIONS.md` when they are made.

- **Genre.** ~~Colony sim, roguelike or hybrid?~~ Decided: an action roguelike with a player
  character (D-031).
- **Agents.** A single avatar, the player (D-031). Enemies that shoot back are wanted (roadmap
  M6b); they would use the creature engine (bodies that push gas and water around and breathe).
  Colonists who carry out orders are not planned.
- **Physical scale.** ~~How big is one pixel?~~ Decided: **5 cm** (`DECISIONS.md` D-037; 10 cm
  was considered for speed and rejected). It sets character size, gas masses and heat time
  scales.
- **Gas model.** ~~One particle per pixel (Noita) or a mass per pixel with pressure (ONI)?~~
  Decided: each gas pixel holds a mass of each gas, with pressure (D-024, D-035). How gas moves
  is being reworked for speed (D-036, proposed).
- **World.** Is the world one fixed-size map or streamed and procedurally generated? Do worlds
  persist?
- **Progression.** What does the player want, and what kills them or the colony?
