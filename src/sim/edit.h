#pragma once

#include "sim/world.h"

typedef enum EditShape {
    EDIT_CIRCLE = 0,
    EDIT_SQUARE = 1,
} EditShape;

// One brush stroke segment, from (x0, y0) to (x1, y1) in world cells. A single click has
// x0 == x1 and y0 == y1. Radius 0 is a single pixel.
typedef struct Edit {
    int32_t x0, y0, x1, y1;
    int32_t radius;
    uint8_t shape;     // EditShape
    ElementId element; // ELEM_VOID erases
    uint8_t only_void; // only replace void cells
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
