#include "generator.h"
#include "finders.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    int cx, cz;
    int bx, bz;
    float y;
    int biome;
} Well;

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <seed> <minY> <maxY>\n",
            argv[0]);
        fprintf(stderr,
            "Example: %s 1520815389707 90 100\n",
            argv[0]);
        return 1;
    }

    uint64_t seed = strtoull(argv[1], NULL, 10);
    float minY = strtof(argv[2], NULL);
    float maxY = strtof(argv[3], NULL);

    if (minY > maxY) {
        fprintf(stderr, "Invalid Y range: minY > maxY\n");
        return 1;
    }

    const int chunks[][2] = {
        {676297, -479950},
        {676297, -479949},
        {676297, -479947},
        {676298, -479950},
        {676298, -479949}
    };

    const int WELL_COUNT =
        sizeof(chunks) / sizeof(chunks[0]);

    Generator g;
    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    SurfaceNoise sn;
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    printf("========================================\n");
    printf(" WELL Y FILTER TEST\n");
    printf("========================================\n");
    printf("Seed       : %llu\n",
        (unsigned long long)seed);
    printf("Version    : MC_26_40\n");
    printf("Wells      : %d\n", WELL_COUNT);
    printf("----------------------------------------\n");
    printf("Y minimum  : %.3f\n", minY);
    printf("Y maximum  : %.3f\n", maxY);
    printf("----------------------------------------\n");

    int totalCandidates = 0;
    int yMatches = 0;

    for (int i = 0; i < WELL_COUNT; i++)
    {
        int cx = chunks[i][0];
        int cz = chunks[i][1];

        Pos p;

        int found = getStructurePos(
            Desert_Well,
            MC_26_40,
            seed,
            cx,
            cz,
            &p
        );

        if (!found) {
            printf("WELL #%d : NOT FOUND\n", i + 1);
            printf("----------------------------------------\n");
            continue;
        }

        int bx = p.x;
        int bz = p.z;

        int biome = getBiomeAt(
            &g,
            4,
            bx >> 2,
            0,
            bz >> 2
        );

        if (biome != desert) {
            printf("WELL #%d : NON-DESERT BIOME (%d)\n",
                i + 1, biome);
            printf("----------------------------------------\n");
            continue;
        }

        float y[1];

        int ret = mapApproxHeight(
            y,
            NULL,
            &g,
            &sn,
            bx >> 2,
            bz >> 2,
            1,
            1
        );

        printf("WELL #%d\n", i + 1);
        printf("Chunk      : (%d,%d)\n", cx, cz);
        printf("Block      : (%d,%d)\n", bx, bz);
        printf("Sample     : (%d,%d)\n",
            bx >> 2, bz >> 2);
        printf("Biome ID   : %d\n", biome);
        printf("Approx Y   : %.3f\n", y[0]);
        printf("Return     : %d\n", ret);

        if (y[0] >= minY && y[0] <= maxY) {
            printf("Y FILTER   : PASS\n");
            yMatches++;
        } else {
            printf("Y FILTER   : FAIL\n");
        }

        printf("----------------------------------------\n");

        totalCandidates++;
    }

    printf("========================================\n");
    printf("Y FILTER TEST COMPLETE\n");
    printf("========================================\n");
    printf("Total candidates : %d\n", totalCandidates);
    printf("Y-range matches  : %d\n", yMatches);
    printf("========================================\n");

    return 0;
}
