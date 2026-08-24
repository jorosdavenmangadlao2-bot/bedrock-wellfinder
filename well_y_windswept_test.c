#include "generator.h"
#include "finders.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

static const char *biome_name(int b)
{
    if (b == desert)
        return "Desert";

#ifdef windswept_savanna
    if (b == windswept_savanna)
        return "Windswept Savanna";
#endif

    return "Other";
}

int main(void)
{
    const uint64_t seed = 1520815444168ULL;
    const int cx = 676300;
    const int cz = -479960;

    const int SEARCH_RADIUS = 8;

    Generator g;
    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    SurfaceNoise sn;
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    Pos p;

    int found = getStructurePos(
        Desert_Well,
        MC_26_40,
        seed,
        cx,
        cz,
        &p
    );

    printf("========================================\n");
    printf(" WELL Y WINDSWEPT SAVANNA TEST\n");
    printf("========================================\n");
    printf("Seed       : %llu\n",
        (unsigned long long)seed);
    printf("Well chunk : (%d,%d)\n", cx, cz);
    printf("Version    : MC_26_40\n");
    printf("----------------------------------------\n");

    if (!found)
    {
        printf("Desert Well candidate: NOT FOUND\n");
        printf("========================================\n");
        return 0;
    }

    int bx = p.x;
    int bz = p.z;

    int wellBiome = getBiomeAt(
        &g,
        4,
        bx >> 2,
        0,
        bz >> 2
    );

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

    printf("Well candidate : FOUND\n");
    printf("Block          : (%d,%d)\n", bx, bz);
    printf("Local          : (%d,%d)\n", bx & 15, bz & 15);
    printf("Well biome ID  : %d (%s)\n",
        wellBiome,
        biome_name(wellBiome));
    printf("Approx Y       : %.3f\n", y[0]);
    printf("----------------------------------------\n");
    printf("SEARCHING NEARBY BIOMES\n");
    printf("Radius         : %d chunks\n", SEARCH_RADIUS);
    printf("----------------------------------------\n");

    int foundWindswept = 0;
    double nearest = 1e30;
    int nearestX = 0;
    int nearestZ = 0;

    for (int z = cz - SEARCH_RADIUS;
         z <= cz + SEARCH_RADIUS;
         z++)
    {
        for (int x = cx - SEARCH_RADIUS;
             x <= cx + SEARCH_RADIUS;
             x++)
        {
            int biome = getBiomeAt(
                &g,
                4,
                x << 2,
                0,
                z << 2
            );

#ifdef windswept_savanna
            if (biome == windswept_savanna)
            {
                foundWindswept = 1;

                double dx = (double)(x - cx);
                double dz = (double)(z - cz);
                double d = sqrt(dx * dx + dz * dz);

                if (d < nearest)
                {
                    nearest = d;
                    nearestX = x;
                    nearestZ = z;
                }
            }
#endif
        }
    }

    printf("----------------------------------------\n");

    if (foundWindswept)
    {
        printf("Windswept Savanna : FOUND\n");
        printf("Nearest chunk     : (%d,%d)\n",
            nearestX, nearestZ);
        printf("Chunk distance    : %.3f\n", nearest);
    }
    else
    {
        printf("Windswept Savanna : NOT FOUND\n");
    }

    printf("----------------------------------------\n");

    if (wellBiome == desert)
        printf("DESERT WELL VALIDATION : PASS\n");
    else
        printf("DESERT WELL VALIDATION : FAIL\n");

    if (foundWindswept)
        printf("WINDSWEPT PROXIMITY    : PASS\n");
    else
        printf("WINDSWEPT PROXIMITY    : NOT FOUND\n");

    printf("========================================\n");

    return 0;
}
