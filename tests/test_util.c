#include "test.h"

#include "sim/particle.h"

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

// The species that holds most of the moles of gas cell i.
static int dominant_species(const World *w, size_t i) {
    int best = 0;
    float most = -1.0f;
    for (int s = 0; s < GAS_SPECIES; s++) {
        float n = w->gas[i].m[s] * g_species_inv_molar[s];
        if (n > most) {
            most = n;
            best = s;
        }
    }
    return best;
}

// Does cell i count as element e? For a gas species: a gas cell mostly of it.
static bool cell_counts_as(const World *w, size_t i, ElementId e) {
    ElementId c = w->cells[i].element;
    int s = elem_species(e);
    if (s >= 0) {
        return c == ELEM_GAS && dominant_species(w, i) == s;
    }
    return c == e;
}

bool test_cell_is(const World *w, int x, int y, ElementId e) {
    return world_in_bounds(w, x, y) ? cell_counts_as(w, world_index(w, x, y), e) : e == ELEM_OUT;
}

int64_t test_count(const World *w, ElementId e) {
    int64_t n = 0;
    size_t total = (size_t)w->width * w->height;
    for (size_t i = 0; i < total; i++) {
        n += cell_counts_as(w, i, e);
    }
    for (int i = 0; i < w->particle_count; i++) { // in flight
        n += w->particles[i].cell.element == e;
    }
    return n;
}

double test_mean_y(const World *w, ElementId e) {
    int s = elem_species(e);
    double sum = 0.0, n = 0.0;
    for (int y = 0; y < w->height; y++) {
        for (int x = 0; x < w->width; x++) {
            size_t i = world_index(w, x, y);
            double weight = 0.0;
            if (s >= 0) {
                weight = w->cells[i].element == ELEM_GAS ? (double)w->gas[i].m[s] : 0.0;
            } else {
                weight = w->cells[i].element == e ? 1.0 : 0.0;
            }
            sum += y * weight;
            n += weight;
        }
    }
    return n > 0.0 ? sum / n : -1.0;
}

double test_mass(const World *w, ElementId e) {
    int s = elem_species(e);
    double m = 0.0;
    for (int i = 0; i < w->width * w->height; i++) {
        if (w->cells[i].element == ELEM_GAS) {
            m += s >= 0 ? (double)w->gas[i].m[s] : (double)gas_total(&w->gas[i]);
        }
    }
    return m;
}
