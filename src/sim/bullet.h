#pragma once

// Bullet designs and what they carry (M6b, D-047). docs/SIMULATION.md -> Guns and bullets.
//
// A bullet is drawn by the player: a small grid of pixels, all touching each other. A pixel is
//   - a solid (or powder): it is shot as it is and does direct damage on impact;
//   - a liquid or a gas (with the pressure of its cell, in kPa);
//   - heat: a temperature.
// Liquids, gases and heat are released either on impact (all at once, where the bullet hits) or
// gradually (a percentage of the pixel's matter or heat per cell the bullet flies, along its path;
// what is left is released at the impact). A bullet in flight is a single fast particle (particle.c)
// that carries a payload: the design and how much of each pixel is still to release.

#include "sim/world.h"

#define BULLET_DIM    8 // the grid of a design, pixels
#define BULLET_PIXELS (BULLET_DIM * BULLET_DIM)

typedef enum BulletPixelKind {
    BULLET_EMPTY = 0,
    BULLET_MATTER = 1, // `element`: a solid, powder, liquid or gas
    BULLET_HEAT = 2,   // `temp_k`
} BulletPixelKind;

typedef enum BulletRelease {
    RELEASE_IMPACT = 0,
    RELEASE_GRADUAL = 1,
} BulletRelease;

typedef struct BulletPixel {
    uint8_t kind;    // BulletPixelKind
    ElementId element;
    uint8_t release; // BulletRelease (liquids, gases and heat)
    float kpa;       // gases: the pressure of the cell, kPa
    float percent;   // gradual: percent of it released per cell flown (0.5 to 100)
    float temp_k;    // heat: the temperature
} BulletPixel;

typedef struct BulletDesign {
    BulletPixel px[BULLET_PIXELS]; // row-major, BULLET_DIM wide
} BulletDesign;

// Defaults of the editor.
#define BULLET_DEFAULT_KPA     100.0f
#define BULLET_DEFAULT_PERCENT 1.0f
#define BULLET_DEFAULT_TEMP_K  1273.15f // 1000 degC

// May this element be drawn into a bullet? Solids, powders, liquids and gases that exist (not the
// bodies of creatures, burning wood, or the pseudo gas element).
bool bullet_element_allowed(ElementId e);

// Is the element a solid for the purposes of a bullet (static or powder)?
bool bullet_element_is_solid(ElementId e);

void bullet_design_clear(BulletDesign *d);

// One pixel of iron: the starting gun's bullet.
void bullet_design_iron(BulletDesign *d);

int bullet_design_pixels(const BulletDesign *d);

// Not empty, and every pixel touches another (4-neighbour) so that it is one piece.
bool bullet_design_connected(const BulletDesign *d);

// The direct damage of a bullet on impact: BULLET_SOLID_DAMAGE hit points for each solid pixel,
// scaled by its density against iron's (between 0.1 and 1.5 times).
#define BULLET_SOLID_DAMAGE 12.0f
float bullet_design_damage(const BulletDesign *d);

// Does it carry anything to release (liquid, gas or heat)?
bool bullet_design_has_payload(const BulletDesign *d);

// Does it have a solid pixel?
bool bullet_design_has_solid(const BulletDesign *d);

// ---- Payloads of bullets in flight ----

#define PAYLOAD_MAX 255 // ids 1..PAYLOAD_MAX; 0 means none

typedef struct Payload {
    uint8_t active;
    BulletDesign design;
    float left[BULLET_PIXELS]; // the part (0..1) of each pixel still to release
} Payload;

// Starts a payload for a bullet of `d` (just fired). Returns its id (1..PAYLOAD_MAX), or 0 if none
// is free (the bullet then flies as a plain one).
int payload_start(World *w, const BulletDesign *d);

// The bullet has flown through cell (x, y): releases what its gradual pixels give there.
void payload_trail(World *w, int id, int x, int y);

// The bullet hit something: (x, y) is the cell it was in, (hx, hy) the cell it hit, (ux, uy) its
// direction. Releases all that is left (gases and heat in and around the cell, liquids thrown out
// of it), and frees the payload. Returns whether the bullet has a solid pixel (it goes on as a
// bullet) or not (it is gone).
bool payload_impact(World *w, int id, int x, int y, int hx, int hy, float ux, float uy);

// The direct damage of the payload's bullet.
float payload_damage(const World *w, int id);

// Frees all payloads (a new level).
void payloads_reset(World *w);
