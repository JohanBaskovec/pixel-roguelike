#pragma once

// The gun menu (Tab) and the bullet editor (D-047): the slots of the gun, each the design of a
// bullet shown zoomed, and a Paint-like window to draw one pixel at a time. docs/SIMULATION.md ->
// Guns and bullets.

#include "sim/weapon.h"
#include "ui/ui.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>

typedef enum MenuState {
    MENU_CLOSED = 0,
    MENU_GUN = 1,    // the slots
    MENU_EDITOR = 2, // one bullet
} MenuState;

typedef enum EditorTool {
    TOOL_MATTER = 0, // draws `element`
    TOOL_HEAT = 1,
    TOOL_ERASE = 2,
} EditorTool;

typedef struct GunMenu {
    MenuState state;
    bool click;                  // the left button went down since the last draw
    bool left_down, right_down;  // held
    int slot;                    // the slot being edited
    BulletDesign design;         // the working copy
    int tool;                    // EditorTool
    ElementId element;           // TOOL_MATTER
    int release;                 // BulletRelease of the next pixels drawn
    float kpa;                   // gases
    float percent;               // gradual release
    float temp_c;                // heat, degC
} GunMenu;

void gunmenu_init(GunMenu *m);

static inline bool gunmenu_open(const GunMenu *m) {
    return m->state != MENU_CLOSED;
}

// A key press: Tab opens and closes the gun menu, Esc leaves the editor (without keeping the
// design) or the menu. While a menu is open it takes every key. Returns whether it took it.
bool gunmenu_key(GunMenu *m, SDL_Keycode key);

// Mouse buttons (for the held state and clicks).
void gunmenu_mouse(GunMenu *m, const SDL_Event *ev);

// Draws the open menu over the whole window (w x h) and acts on clicks. Changes `gun`.
void gunmenu_draw(GunMenu *m, Ui *ui, int w, int h, Rifle *gun);
