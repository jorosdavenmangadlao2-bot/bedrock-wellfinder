#include <stdio.h>
#include <stdint.h>
#include "mt.h"

int main(void)
{
    uint64_t worldSeed = 1520815389707ULL;
    int chunkX = 676297;
    int chunkZ = -479950;
    int salt = -1160484816;
    int rarity = 500;

    printf("========================================\n");
    printf(" Desert Well Decoration RNG Audit\n");
    printf("========================================\n");
    printf("Seed      : %llu\n", (unsigned long long)worldSeed);
    printf("Chunk     : (%d, %d)\n", chunkX, chunkZ);
    printf("Salt      : %d\n", salt);
    printf("Rarity    : %d\n\n", rarity);

    if (mt_ws != worldSeed)
    {
        setSeed(worldSeed);
        mt_a = next();
        mt_b = next();
        mt_ws = worldSeed;
    }

    printf("mt_a      : %d\n", mt_a);
    printf("mt_b      : %d\n", mt_b);

    uint32_t seed =
        (chunkX * (mt_a | 1u) +
         chunkZ * (mt_b | 1u)) ^ worldSeed;

    printf("Pre-salt  : %u (0x%08x)\n", seed, seed);

    seed = ((seed >> 2) +
            (seed << 6) +
            salt -
            1640531527u) ^ seed;

    printf("Deco seed : %u (0x%08x)\n", seed, seed);

    setSeed(seed);

    uint32_t raw1 = mt_next();
    int gate = (int)(raw1 % rarity);

    printf("\nRNG #1 raw: %u\n", raw1);
    printf("Gate      : %d\n", gate);

    uint32_t raw2 = mt_next();
    int localZ = (int)(raw2 % 16);

    uint32_t raw3 = mt_next();
    int localX = (int)(raw3 % 16);

    printf("RNG #2 raw: %u\n", raw2);
    printf("Local Z   : %d\n", localZ);

    printf("RNG #3 raw: %u\n", raw3);
    printf("Local X   : %d\n", localX);

    printf("\nExpected observation:\n");
    printf("Local X   : 14\n");
    printf("Local Z   : 11\n");

    printf("\nVERDICT\n");
    if (gate == 0 && localX == 14 && localZ == 11)
        printf("EXACT MATCH\n");
    else
        printf("MISMATCH\n");

    return 0;
}
