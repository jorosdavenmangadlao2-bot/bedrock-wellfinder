#include "generator.h"
#include "finders.h"
#include "biomes.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

#define DEFAULT_RADIUS 100

static void scan_seed(uint64_t seed, int radius)
{
    Generator g;
    SurfaceNoise sn;

    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    int found = 0;

    printf("========================================\n");
    printf(" SINGLE DESERT WELL Y FINDER\n");
    printf("========================================\n");
    printf("Seed       : %" PRIu64 "\n", seed);
    printf("Version    : MC_26_40\n");
    printf("Radius     : %d chunks\n", radius);
    printf("3-well     : DISABLED\n");
    printf("4-well     : DISABLED\n");
    printf("Cluster    : DISABLED\n");
    printf("========================================\n");

    for (int cx = -radius; cx <= radius; cx++)
    {
        for (int cz = -radius; cz <= radius; cz++)
        {
            Pos pos;

            /*
             * Existing Cubiomes Desert Well placement.
             * This checks only ONE chunk at a time.
             */
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

            /*
             * Confirm the biome at the well position.
             * Scale 4 = 1/4 horizontal resolution.
             */
            int biome_id = getBiomeAt(
                &g,
                4,
                pos.x >> 2,
                0,
                pos.z >> 2
            );

            /*
             * Calculate approximate Overworld surface height.
             */
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

            found++;

            printf(
                "WELL #%d  chunk=(%d,%d)  block=(%d,%d)  "
                "Y=%.3f  biome=%d\n",
                found,
                cx,
                cz,
                pos.x,
                pos.z,
                surface_y,
                biome_id
            );
        }
    }

    printf("========================================\n");
    printf("Individual wells found: %d\n", found);
    printf("Physical 3/4-well test: NOT RUN\n");
    printf("========================================\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3)
    {
        fprintf(
            stderr,
            "Usage: %s <seed> [radius]\n",
            argv[0]
        );

        fprintf(
            stderr,
            "Example: %s 1520815389707 100\n",
            argv[0]
        );

        return 1;
    }

    uint64_t seed = strtoull(argv[1], NULL, 10);
    int radius = DEFAULT_RADIUS;

    if (argc == 3)
    {
        radius = atoi(argv[2]);

        if (radius < 0)
        {
            fprintf(stderr, "Invalid radius: %d\n", radius);
            return 1;
        }
    }

    scan_seed(seed, radius);

    return 0;
}
