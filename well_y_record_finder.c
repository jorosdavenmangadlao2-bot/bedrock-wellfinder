#include "generator.h"
#include "finders.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <float.h>

typedef struct
{
    int found;
    int cx;
    int cz;
    int bx;
    int bz;
    int biome;
    float y;
    uint64_t seed;
} BestWell;

static void scan_seed(
    uint64_t seed,
    int centerX,
    int centerZ,
    int radius,
    BestWell *best,
    uint64_t *desertCount)
{
    Generator g;

    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    SurfaceNoise sn;
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    int minX = centerX - radius;
    int maxX = centerX + radius;
    int minZ = centerZ - radius;
    int maxZ = centerZ + radius;

    for (int cz = minZ; cz <= maxZ; cz++)
    {
        for (int cx = minX; cx <= maxX; cx++)
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

            (*desertCount)++;

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

            if (!best->found || y[0] > best->y)
            {
                best->found = 1;
                best->seed = seed;
                best->cx = cx;
                best->cz = cz;
                best->bx = bx;
                best->bz = bz;
                best->biome = biome;
                best->y = y[0];
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 6)
    {
        fprintf(
            stderr,
            "Usage: %s <seed_start> <seed_end> <centerX> <centerZ> <radius>\n",
            argv[0]
        );
        return 1;
    }

    uint64_t seedStart = strtoull(argv[1], NULL, 10);
    uint64_t seedEnd   = strtoull(argv[2], NULL, 10);

    int centerX = atoi(argv[3]);
    int centerZ = atoi(argv[4]);
    int radius  = atoi(argv[5]);

    BestWell best;
    best.found = 0;
    best.y = -FLT_MAX;

    uint64_t totalDesert = 0;
    uint64_t seedsScanned = 0;

    printf("========================================\n");
    printf(" DESERT WELL Y RECORD FINDER\n");
    printf("========================================\n");
    printf("Version    : MC_26_40\n");
    printf("Seed start : %llu\n",
        (unsigned long long)seedStart);
    printf("Seed end   : %llu\n",
        (unsigned long long)seedEnd);
    printf("Center     : (%d,%d)\n", centerX, centerZ);
    printf("Radius     : %d chunks\n", radius);
    printf("----------------------------------------\n");

    for (uint64_t seed = seedStart; seed <= seedEnd; seed++)
    {
        BestWell before = best;

        scan_seed(
            seed,
            centerX,
            centerZ,
            radius,
            &best,
            &totalDesert
        );

        seedsScanned++;

        if (best.found && (!before.found || best.y > before.y))
        {
            printf("\nNEW Y RECORD\n");
            printf("----------------------------------------\n");
            printf("Seed       : %llu\n",
                (unsigned long long)best.seed);
            printf("Chunk      : (%d,%d)\n",
                best.cx, best.cz);
            printf("Block      : (%d,%d)\n",
                best.bx, best.bz);
            printf("Local      : (%d,%d)\n",
                best.bx & 15, best.bz & 15);
            printf("Biome ID   : %d\n", best.biome);
            printf("Approx Y    : %.3f\n", best.y);
            printf("----------------------------------------\n");
        }

        if (seed == UINT64_MAX)
            break;
    }

    printf("\n========================================\n");
    printf(" SEARCH COMPLETE\n");
    printf("========================================\n");
    printf("Seeds scanned : %llu\n",
        (unsigned long long)seedsScanned);
    printf("Desert wells  : %llu\n",
        (unsigned long long)totalDesert);

    if (best.found)
    {
        printf("----------------------------------------\n");
        printf("HIGHEST Y FOUND\n");
        printf("Seed       : %llu\n",
            (unsigned long long)best.seed);
        printf("Chunk      : (%d,%d)\n",
            best.cx, best.cz);
        printf("Block      : (%d,%d)\n",
            best.bx, best.bz);
        printf("Local      : (%d,%d)\n",
            best.bx & 15, best.bz & 15);
        printf("Biome ID   : %d\n", best.biome);
        printf("Approx Y    : %.3f\n", best.y);
    }
    else
    {
        printf("No Desert Wells found.\n");
    }

    printf("========================================\n");

    return 0;
}
