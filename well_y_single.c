#include "generator.h"
#include "finders.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr,
            "Usage: %s <seed> <chunkX> <chunkZ>\n",
            argv[0]);
        return 1;
    }

    uint64_t seed = strtoull(argv[1], NULL, 10);
    int chunkX = atoi(argv[2]);
    int chunkZ = atoi(argv[3]);

    Generator g;
    SurfaceNoise sn;

    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    Pos pos;

    int result = getStructurePos(
        Desert_Well,
        MC_26_40,
        seed,
        chunkX,
        chunkZ,
        &pos
    );

    printf("========================================\n");
    printf(" SINGLE DESERT WELL VALIDATOR\n");
    printf("========================================\n");
    printf("Seed       : %" PRIu64 "\n", seed);
    printf("Chunk X    : %d\n", chunkX);
    printf("Chunk Z    : %d\n", chunkZ);
    printf("----------------------------------------\n");

    if (!result)
    {
        printf("Well candidate: NONE\n");
        printf("========================================\n");
        return 0;
    }

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

    printf("Well candidate: FOUND\n");
    printf("Block X       : %d\n", pos.x);
    printf("Block Z       : %d\n", pos.z);
    printf("Biome ID      : %d\n", biome_id);
    printf("Surface biome : %d\n", surface_biome);
    printf("Approx Y      : %.3f\n", surface_y);
    printf("----------------------------------------\n");
    printf("3-well logic  : DISABLED\n");
    printf("4-well logic  : DISABLED\n");
    printf("Cluster logic : DISABLED\n");
    printf("========================================\n");

    return 0;
}
