#include "generator.h"
#include "finders.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

static void scan_rect(
    uint64_t seed,
    int startX,
    int startZ,
    int width,
    int height)
{
    Generator g;
    SurfaceNoise sn;

    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    int candidates = 0;
    int desert_candidates = 0;

    printf("========================================\n");
    printf(" SINGLE DESERT WELL RECT SCANNER\n");
    printf("========================================\n");
    printf("Seed       : %" PRIu64 "\n", seed);
    printf("Version    : MC_26_40\n");
    printf("Start      : (%d,%d)\n", startX, startZ);
    printf("Size       : %d x %d chunks\n", width, height);
    printf("----------------------------------------\n");
    printf("3-well     : DISABLED\n");
    printf("4-well     : DISABLED\n");
    printf("Cluster    : DISABLED\n");
    printf("========================================\n");

    for (int dx = 0; dx < width; dx++)
    {
        for (int dz = 0; dz < height; dz++)
        {
            int cx = startX + dx;
            int cz = startZ + dz;

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

            if (biome_id != desert)
                continue;

            desert_candidates++;

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

            printf(
                "WELL #%d\n"
                "  Chunk : (%d,%d)\n"
                "  Block : (%d,%d)\n"
                "  Local : (%d,%d)\n"
                "  Biome : %d\n"
                "  Y     : %.3f\n"
                "----------------------------------------\n",
                desert_candidates,
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
    printf("Total structure candidates : %d\n", candidates);
    printf("Desert biome candidates    : %d\n", desert_candidates);
    printf("3/4-well search             : NOT RUN\n");
    printf("========================================\n");
}

int main(int argc, char **argv)
{
    if (argc != 6)
    {
        fprintf(
            stderr,
            "Usage: %s <seed> <startX> <startZ> <width> <height>\n",
            argv[0]
        );

        fprintf(
            stderr,
            "Example: %s 1520815389707 676287 -479960 21 21\n",
            argv[0]
        );

        return 1;
    }

    uint64_t seed = strtoull(argv[1], NULL, 10);
    int startX = atoi(argv[2]);
    int startZ = atoi(argv[3]);
    int width = atoi(argv[4]);
    int height = atoi(argv[5]);

    if (width <= 0 || height <= 0)
    {
        fprintf(stderr, "Invalid rectangle size.\n");
        return 1;
    }

    scan_rect(seed, startX, startZ, width, height);

    return 0;
}
