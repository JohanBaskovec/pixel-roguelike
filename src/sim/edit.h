#pragma once

#include "sim/world.h"

typedef enum EditShape {
    EDIT_CIRCLE = 0,
    EDIT_SQUARE = 1,
} EditShape;

typedef enum EditMode {
    EDIT_PAINT = 0, // put `element` in the cells
    EDIT_HEAT = 1,  // add EDIT_HEAT_STEP_K to every non-void cell, each application
    EDIT_COOL = 2,  // remove EDIT_HEAT_STEP_K
} EditMode;

#define EDIT_HEAT_STEP_K 8.0f
#define EDIT_HEAT_MAX_K  5000.0f

// One brush stroke segment, from (x0, y0) to (x1, y1) in world cells. A single click has
// x0 == x1 and y0 == y1. Radius 0 is a single pixel.
typedef struct Edit {
    int32_t x0, y0, x1, y1;
    int32_t radius;
    uint8_t shape;     // EditShape
    ElementId element; // EDIT_PAINT: ELEM_VOID erases
    uint8_t only_void; // EDIT_PAINT: only replace void cells
    uint8_t mode;      // EditMode
} Edit;

#define EDIT_QUEUE_CAP 256

// Player edits are queued and applied on the main thread between ticks, never during one.
typedef struct EditQueue {
    Edit items[EDIT_QUEUE_CAP];
    int count;
} EditQueue;

// Returns false if the queue is full (apply it, then push again).
bool edit_queue_push(EditQueue *q, const Edit *e);

// Applies every queued edit in order, then empties the queue.
void sim_apply_edits(World *w, EditQueue *q);

// Applies one edit immediately. Returns the number of cells written.
int64_t sim_apply_edit(World *w, const Edit *e);
