#include "generator.h"
#include "finders.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

typedef struct
{
    int cx, cz;
    int bx, bz;
    float y;
} Well;

static double dist(const Well *a, const Well *b)
{
    double dx = (double)a->bx - (double)b->bx;
    double dz = (double)a->bz - (double)b->bz;
    return sqrt(dx * dx + dz * dz);
}

static int physical_group(const Well *w, int n, double limit)
{
    for (int i = 0; i < n; i++)
    {
        for (int j = i + 1; j < n; j++)
        {
            if (dist(&w[i], &w[j]) > limit)
                return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <seed>\n", argv[0]);
        return 1;
    }

    uint64_t seed = strtoull(argv[1], NULL, 10);

    const int startX = 676287;
    const int startZ = -479960;
    const int sizeX = 21;
    const int sizeZ = 21;
    const double LIMIT = 32.0;

    Well wells[512];
    int count = 0;

    Generator g;
    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    printf("========================================\n");
    printf(" AUTOMATIC PHYSICAL WELL SCANNER\n");
    printf("========================================\n");
    printf("Seed       : %llu\n",
           (unsigned long long)seed);
    printf("Version    : MC_26_40\n");
    printf("Start      : (%d,%d)\n", startX, startZ);
    printf("Size       : %dx%d chunks\n", sizeX, sizeZ);
    printf("Distance   : %.1f blocks\n", LIMIT);
    printf("----------------------------------------\n");

    for (int cz = startZ; cz < startZ + sizeZ; cz++)
    {
        for (int cx = startX; cx < startX + sizeX; cx++)
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

            if (count >= 512)
                continue;

            wells[count].cx = cx;
            wells[count].cz = cz;
            wells[count].bx = bx;
            wells[count].bz = bz;
            wells[count].y = 0.0f;

            count++;

            printf("WELL #%d  chunk=(%d,%d) block=(%d,%d) biome=%d\n",
                   count, cx, cz, bx, bz, biome);
        }
    }

    printf("========================================\n");
    printf("Total desert wells : %d\n", count);
    printf("========================================\n");

    int groups3 = 0;
    int groups4 = 0;

    for (int a = 0; a < count; a++)
    {
        for (int b = a + 1; b < count; b++)
        {
            for (int c = b + 1; c < count; c++)
            {
                Well group3[3] = {
                    wells[a], wells[b], wells[c]
                };

                if (physical_group(group3, 3, LIMIT))
                {
                    groups3++;

                    printf(
                        "3-WELL GROUP #%d: "
                        "(%d,%d) (%d,%d) (%d,%d)\n",
                        groups3,
                        wells[a].bx, wells[a].bz,
                        wells[b].bx, wells[b].bz,
                        wells[c].bx, wells[c].bz
                    );
                }

                for (int d = c + 1; d < count; d++)
                {
                    Well group4[4] = {
                        wells[a], wells[b],
                        wells[c], wells[d]
                    };

                    if (physical_group(group4, 4, LIMIT))
                    {
                        groups4++;

                        printf(
                            "4-WELL GROUP #%d: "
                            "(%d,%d) (%d,%d) "
                            "(%d,%d) (%d,%d)\n",
                            groups4,
                            wells[a].bx, wells[a].bz,
                            wells[b].bx, wells[b].bz,
                            wells[c].bx, wells[c].bz,
                            wells[d].bx, wells[d].bz
                        );
                    }
                }
            }
        }
    }

    printf("========================================\n");
    printf("PHYSICAL GROUP RESULT\n");
    printf("========================================\n");
    printf("Physical 3-well groups : %d\n", groups3);
    printf("Physical 4-well groups : %d\n", groups4);
    printf("========================================\n");

    return 0;
}
