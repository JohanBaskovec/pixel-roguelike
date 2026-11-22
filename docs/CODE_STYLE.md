# Code style (C17)

`.clang-format` at the repository root is the source of truth for formatting. Run
`clang-format -i` on the files you touch. This doc covers everything a formatter cannot.

## Language

- **C17.** Compile with `-std=c17` (GCC/Clang) or `/std:c17` (MSVC). No compiler extensions in
  portable code. Where an extension is worth using (for example `__builtin_expect`), wrap it in a
  macro in `core/common.h` with a fallback.
- **No VLAs**, no `alloca`, no `goto` except for single-exit cleanup.
- Use `<stdint.h>` fixed-width types for data (`uint8_t`, `int32_t`, ...) and `int` / `size_t`
  for loop counters and sizes as appropriate. Use `bool` from `<stdbool.h>`.
- Do not use C11 `<threads.h>` or `<stdatomic.h>`. Use the wrappers in `core/` (SDL3 threads and
  atomics). MSVC support for the C11 versions is unreliable.

## Naming

| Thing | Convention | Example |
|---|---|---|
| Functions | `module_verb_noun`, snake_case | `world_create`, `sim_tick`, `ui_button` |
| Types (struct, enum, typedef) | PascalCase | `World`, `Cell`, `EditQueue` |
| Enum values, macros, constants | UPPER_SNAKE with a prefix | `ELEM_WATER`, `BEHAVIOR_GAS`, `CHUNK_SIZE` |
| Globals | `g_` prefix; rare; read-only tables preferred | `g_elem_density` |
| File-local functions and data | `static`, no prefix needed | `static void liquid_spread(...)` |
| Files | snake_case, one module per `.c`/`.h` pair | `sim/heat.c`, `sim/heat.h` |

## Files and headers

- `#pragma once` at the top of every header.
- A header exposes only what other modules need. Everything else is `static` in the `.c` file.
- Include order: the module's own header, then project headers, then SDL, then the standard
  library.
- Hot helpers that must inline go in the header as `static inline`.

## Formatting (summary of .clang-format)

4-space indent and no tabs; 100-column limit; braces on the same line (K&R/1TBS); always use braces,
even for one-line `if`s; pointer star attached to the name (`Cell *cells`).

## Errors, asserts, logging

- **Invariants** use `PR_ASSERT(cond)`. It is active in Debug, compiled out in Release, and allowed
  in hot code. `PR_ASSERT_ALWAYS(cond)` is for cheap checks that must stay in Release.
- **Recoverable errors** (file I/O, SDL init) return `bool` or an error enum and log with
  `pr_log_error(...)`, which wraps `SDL_Log`. Never silently ignore an SDL return value.
- No logging in hot code.

## Memory

- Allocate large buffers once, at startup or at world creation, with `pr_aligned_alloc(64, size)`.
- The module that allocates a buffer owns it and frees it in its `_destroy` function.
- No allocation inside the tick. If a per-tick scratch buffer is needed, preallocate it per
  worker.

## Hot path

See the rules in `CLAUDE.md` → Hot path. In short: no allocation, locks, logging, SDL calls or
function pointers per cell; table lookups only; respect `MAX_MOVE`; use the chunk RNG only.

## Tests

- A tiny in-repo framework (`tests/test.h`: `TEST(name)`, `EXPECT(cond)`, `EXPECT_EQ(a, b)`) run
  through CTest. No external test dependency.
- Behaviour tests build small worlds (for example 64×64) from an ASCII sketch, run N ticks with a
  fixed seed, and assert on the result. For example:

  ```c
  World *w = test_world_from_ascii(
      "#######\n"
      "#  ~  #\n"     // ~ water, o oxygen, c CO2, h hydrogen, # dirt, ' ' void
      "#     #\n"
      "#######\n");
  sim_run_ticks(w, 200);
  EXPECT(test_cell_is(w, 3, 2, ELEM_WATER));   // water fell to the floor
  ```

## Warnings

- GCC/Clang: `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wdouble-promotion`
- MSVC: `/W4`
- CI builds with warnings as errors (`-Werror` / `/WX`). Local builds do not, so prototyping is
  not blocked.

## Commits

- Imperative, present-tense subject line (≤ 72 characters), for example
  `sim: add liquid dispersion`.
- Prefix with the module: `sim:`, `render:`, `ui:`, `game:`, `core:`, `build:`, `docs:`.
- Each commit builds and passes tests. Docs are updated in the same commit as the change they
  describe.
