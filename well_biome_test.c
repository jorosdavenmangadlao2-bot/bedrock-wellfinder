#include <stdio.h>
#include <stdint.h>
#include "finders.h"
#include "generator.h"

int main(void)
{
    uint64_t seed = 1520815389707ULL;

    Generator g;
    setupGenerator(&g, MC_1_21_60, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    int chunks[][2] = {
        {676297, -479950},
        {676298, -479950},
        {676297, -479949},
        {676298, -479949}
    };

    printf("========================================\n");
    printf(" Desert Well + Biome Validation\n");
    printf("========================================\n");
    printf("Seed: %llu\n\n",
           (unsigned long long)seed);

    for (int i = 0; i < 4; i++)
    {
        Pos p;

        int found = getStructurePos(
            Desert_Well,
            MC_1_21_60,
            seed,
            chunks[i][0],
            chunks[i][1],
            &p
        );

        printf("Case %d\n", i + 1);
        printf("Chunk: (%d, %d)\n",
               chunks[i][0], chunks[i][1]);

        if (!found)
        {
            printf("RNG candidate: NO\n");
            printf("Result: NO WELL\n\n");
            continue;
        }

        printf("RNG candidate: YES\n");
        printf("World X: %d\n", p.x);
        printf("World Z: %d\n", p.z);

        int biomeOK = isViableStructurePos(
            Desert_Well,
            &g,
            p.x,
            p.z,
            0
        );

        int biomeID = getBiomeAt(
            &g,
            4,
            p.x >> 2,
            0,
            p.z >> 2
        );

        printf("Biome ID: %d\n", biomeID);
        printf("Biome validation: %s\n",
               biomeOK ? "PASS" : "FAIL");

        printf("----------------------------------------\n\n");
    }

    return 0;
}
