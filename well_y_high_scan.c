#include "generator.h"
#include "finders.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 6)
    {
        fprintf(stderr,
            "Usage: %s <seed> <startX> <startZ> <size> <minY>\n",
            argv[0]);
        return 1;
    }

    uint64_t seed = strtoull(argv[1], NULL, 10);
    int startX = atoi(argv[2]);
    int startZ = atoi(argv[3]);
    int size = atoi(argv[4]);
    float minY = strtof(argv[5], NULL);

    Generator g;
    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    SurfaceNoise sn;
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    int structureCount = 0;
    int desertCount = 0;
    int highYCount = 0;

    printf("========================================\n");
    printf(" HIGH-Y DESERT WELL SCANNER\n");
    printf("========================================\n");
    printf("Seed       : %llu\n",
        (unsigned long long)seed);
    printf("Version    : MC_26_40\n");
    printf("Start      : (%d,%d)\n", startX, startZ);
    printf("Size       : %d x %d chunks\n", size, size);
    printf("Minimum Y  : %.3f\n", minY);
    printf("----------------------------------------\n");

    for (int cz = startZ; cz < startZ + size; cz++)
    {
        for (int cx = startX; cx < startX + size; cx++)
        {
            Pos p;

            int found = getStructurePos(
                Desert_Well,
                MC_26_40,
                seed,
                cx,
                cz,
                &p
            );

            if (!found)
                continue;

            structureCount++;

            int bx = p.x;
            int bz = p.z;

            int biome = getBiomeAt(
                &g,
                4,
                bx >> 2,
                0,
                bz >> 2
            );

            if (biome != desert)
                continue;

            desertCount++;

            float y[1];

            mapApproxHeight(
                y,
                NULL,
                &g,
                &sn,
                bx >> 2,
                bz >> 2,
                1,
                1
            );

            if (y[0] < minY)
                continue;

            highYCount++;

            printf(
                "HIGH-Y WELL #%d  chunk=(%d,%d) "
                "block=(%d,%d) local=(%d,%d) "
                "Y=%.3f biome=%d\n",
                highYCount,
                cx,
                cz,
                bx,
                bz,
                bx & 15,
                bz & 15,
                y[0],
                biome
            );
        }
    }

    printf("========================================\n");
    printf("HIGH-Y SEARCH COMPLETE\n");
    printf("========================================\n");
    printf("Structure candidates : %d\n", structureCount);
    printf("Desert wells          : %d\n", desertCount);
    printf("Y >= %.3f matches      : %d\n",
        minY, highYCount);
    printf("========================================\n");

    return 0;
}
