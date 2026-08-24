#include "generator.h"
#include "finders.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#define DEFAULT_RADIUS 100
#define MIN_Y 80.0f
#define MAX_Y 100.0f

static void scan_seed(uint64_t seed, int radius)
{
    Generator g;
    SurfaceNoise sn;

    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    int candidates = 0;
    int y_matches = 0;

    printf("========================================\n");
    printf(" SINGLE DESERT WELL Y SCANNER\n");
    printf("========================================\n");
    printf("Seed       : %" PRIu64 "\n", seed);
    printf("Version    : MC_26_40\n");
    printf("Radius     : %d chunks\n", radius);
    printf("Y range    : %.1f - %.1f\n", MIN_Y, MAX_Y);
    printf("----------------------------------------\n");
    printf("3-well     : DISABLED\n");
    printf("4-well     : DISABLED\n");
    printf("Cluster    : DISABLED\n");
    printf("========================================\n");

    for (int cx = -radius; cx <= radius; cx++)
    {
        for (int cz = -radius; cz <= radius; cz++)
        {
            Pos pos;

            if (!getStructurePos(
                    Desert_Well,
                    MC_26_40,
                    seed,
                    cx,
                    cz,
                    &pos))
            {
                continue;
            }

            candidates++;

            int biome_id = getBiomeAt(
                &g,
                4,
                pos.x >> 2,
                0,
                pos.z >> 2
            );

            float surface_y = 0.0f;
            int surface_biome = 0;

            mapApproxHeight(
                &surface_y,
                &surface_biome,
                &g,
                &sn,
                pos.x >> 2,
                pos.z >> 2,
                1,
                1
            );

            /*
             * Require Desert biome.
             *
             * biome ID 2 is Desert in the current Cubiomes
             * biome registry.
             */
            if (biome_id != desert)
                continue;

            if (surface_y < MIN_Y || surface_y > MAX_Y)
                continue;

            y_matches++;

            printf(
                "MATCH #%d\n"
                "  Chunk : (%d,%d)\n"
                "  Block : (%d,%d)\n"
                "  Local : (%d,%d)\n"
                "  Biome : %d\n"
                "  Y     : %.3f\n"
                "----------------------------------------\n",
                y_matches,
                cx,
                cz,
                pos.x,
                pos.z,
                pos.x & 15,
                pos.z & 15,
                biome_id,
                surface_y
            );
        }
    }

    printf("========================================\n");
    printf("Total candidates : %d\n", candidates);
    printf("Y-range matches  : %d\n", y_matches);
    printf("3/4-well search  : NOT RUN\n");
    printf("========================================\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
    {
        fprintf(stderr,
            "Usage: %s <seed> [radius]\n",
            argv[0]);

        fprintf(stderr,
            "Example: %s 1520815389707 10\n",
            argv[0]);

        return 1;
    }

    uint64_t seed = strtoull(argv[1], NULL, 10);
    int radius = DEFAULT_RADIUS;

    if (argc == 3)
        radius = atoi(argv[2]);

    if (radius < 0)
    {
        fprintf(stderr, "Invalid radius: %d\n", radius);
        return 1;
    }

    scan_seed(seed, radius);

    return 0;
}
