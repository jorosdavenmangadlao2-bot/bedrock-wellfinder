#include <stdio.h>
#include "finders.h"

int main(void)
{
    uint64_t seed = 1520815389707ULL;
    int chunkX = 676297;
    int chunkZ = -479950;

    Pos p;

    int ok = getStructurePos(
        Desert_Well,
        MC_26_40,
        seed,
        chunkX,
        chunkZ,
        &p
    );

    printf("========================================\n");
    printf(" Cubiomes Desert Well Direct Test\n");
    printf("========================================\n");
    printf("Seed  : %llu\n", (unsigned long long)seed);
    printf("Chunk : (%d, %d)\n", chunkX, chunkZ);
    printf("Found : %d\n", ok);

    if (ok)
    {
        printf("World X : %d\n", p.x);
        printf("World Z : %d\n", p.z);
        printf("Local X : %d\n", p.x & 15);
        printf("Local Z : %d\n", p.z & 15);
    }

    return 0;
}
