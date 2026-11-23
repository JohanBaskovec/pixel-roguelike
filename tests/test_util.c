#include "test.h"

#include <string.h>

static ElementId elem_from_char(char ch) {
    switch (ch) {
        case '#':
            return ELEM_DIRT;
        case 'R':
            return ELEM_ROCK;
        case 'I':
            return ELEM_IRON;
        case 'i':
            return ELEM_ICE;
        case '~':
            return ELEM_WATER;
        case 'o':
            return ELEM_OXYGEN;
        case 'c':
            return ELEM_CO2;
        case 'h':
            return ELEM_HYDROGEN;
        default:
            return ELEM_VOID;
    }
}

World *test_world_from_ascii(const char *ascii, uint64_t seed) {
    int w = 0, h = 0, line = 0;
    for (const char *p = ascii; *p; p++) {
        if (*p == '\n') {
            h++;
            line = 0;
        } else {
            line++;
            if (line > w) {
                w = line;
            }
        }
    }
    if (line > 0) {
        h++;
    }
    World *world = world_create(w, h, seed);
    if (!world) {
        return NULL;
    }
    for (int y = 0; y < world->height; y++) {
        for (int x = 0; x < world->width; x++) {
            world_set_cell(world, x, y, ELEM_DIRT, (uint32_t)(x * 31 + y));
        }
    }
    int x = 0, y = 0;
    for (const char *p = ascii; *p; p++) {
        if (*p == '\n') {
            y++;
            x = 0;
            continue;
        }
        world_set_cell(world, x, y, elem_from_char(*p), (uint32_t)(x * 31 + y));
        x++;
    }
    world_wake_rect(world, 0, 0, world->width - 1, world->height - 1);
    world_wake_heat(world, 0, 0, world->width - 1, world->height - 1);
    return world;
}

World *test_world_box(int w, int h, uint64_t seed) {
    World *world = world_create(w, h, seed);
    if (!world) {
        return NULL;
    }
    for (int y = 0; y < world->height; y++) {
        for (int x = 0; x < world->width; x++) {
            bool wall = x == 0 || y == 0 || x == world->width - 1 || y == world->height - 1;
            world_set_cell(world, x, y, wall ? ELEM_DIRT : ELEM_VOID, (uint32_t)(x + y));
        }
    }
    world_wake_rect(world, 0, 0, world->width - 1, world->height - 1);
    world_wake_heat(world, 0, 0, world->width - 1, world->height - 1);
    return world;
}

bool test_cell_is(const World *w, int x, int y, ElementId e) {
    return world_elem(w, x, y) == e;
}

int64_t test_count(const World *w, ElementId e) {
    int64_t n = 0;
    size_t total = (size_t)w->width * w->height;
    for (size_t i = 0; i < total; i++) {
        n += w->cells[i].element == e;
    }
    return n;
}

double test_mean_y(const World *w, ElementId e) {
    bool gas = g_elem_behavior[e] == BEHAVIOR_GAS;
    double sum = 0.0, n = 0.0;
    for (int y = 0; y < w->height; y++) {
        for (int x = 0; x < w->width; x++) {
            size_t i = world_index(w, x, y);
            if (w->cells[i].element == e) {
                double weight = gas ? (double)w->mass[i] : 1.0;
                sum += y * weight;
                n += weight;
            }
        }
    }
    return n > 0.0 ? sum / n : -1.0;
}

double test_mass(const World *w, ElementId e) {
    double m = 0.0;
    for (int i = 0; i < w->width * w->height; i++) {
        if (w->cells[i].element == e) {
            m += (double)w->mass[i];
        }
    }
    return m;
}
