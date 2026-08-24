#include "generator.h"
#include "finders.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

typedef struct {
    int cx, cz;
    int bx, bz;
    float y;
} Well;

static double dist2(const Well *a, const Well *b)
{
    double dx = (double)a->bx - b->bx;
    double dz = (double)a->bz - b->bz;
    return dx * dx + dz * dz;
}

int main(void)
{
    const uint64_t seed = 1520815389707ULL;

    Generator g;
    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    SurfaceNoise sn;
    initSurfaceNoise(&sn, DIM_OVERWORLD, seed);

    /*
     * Known quadwell area.
     * This is ONLY a validation test.
     */
    const int startX = 676287;
    const int startZ = -479960;
    const int sizeX = 21;
    const int sizeZ = 21;

    Well wells[256];
    int count = 0;

    printf("========================================\n");
    printf(" PHYSICAL DESERT WELL VALIDATION TEST\n");
    printf("========================================\n");
    printf("Seed : %llu\n",
        (unsigned long long)seed);
    printf("Area : (%d,%d) %dx%d chunks\n",
        startX, startZ, sizeX, sizeZ);
    printf("----------------------------------------\n");

    for (int cz = startZ; cz < startZ + sizeZ; cz++)
    {
        for (int cx = startX; cx < startX + sizeX; cx++)
        {
            Pos p = getStructurePos(
                Desert_Well,
                &g,
                cx,
                cz,
                0
            );

            /*
             * getStructurePos() gives the candidate position.
             * Verify that the biome is actually desert.
             */
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

            float y;
            int id;

            mapApproxHeight(
                &y,
                &id,
                &g,
                &sn,
                bx >> 2,
                bz >> 2,
                1,
                1
            );

            if (count >= 256)
                continue;

            wells[count].cx = cx;
            wells[count].cz = cz;
            wells[count].bx = bx;
            wells[count].bz = bz;
            wells[count].y  = y;

            printf("WELL #%d\n", count + 1);
            printf(" Chunk : (%d,%d)\n", cx, cz);
            printf(" Block : (%d,%d)\n", bx, bz);
            printf(" Local : (%d,%d)\n",
                bx & 15,
                bz & 15);
            printf(" Y     : %.3f\n", y);
            printf("----------------------------------------\n");

            count++;
        }
    }

    printf("========================================\n");
    printf("Total desert wells : %d\n", count);
    printf("========================================\n\n");

    /*
     * Physical 3-well test.
     *
     * We deliberately use BLOCK coordinates,
     * not chunk adjacency.
     */
    printf("PHYSICAL 3-WELL TEST\n");
    printf("----------------------------------------\n");

    int found3 = 0;

    for (int a = 0; a < count; a++)
    {
        for (int b = a + 1; b < count; b++)
        {
            for (int c = b + 1; c < count; c++)
            {
                double ab = sqrt(dist2(&wells[a], &wells[b]));
                double ac = sqrt(dist2(&wells[a], &wells[c]));
                double bc = sqrt(dist2(&wells[b], &wells[c]));

                /*
                 * Physical proximity threshold.
                 * This is intentionally generous for validation.
                 */
                const double LIMIT = 32.0;

                if (ab <= LIMIT &&
                    ac <= LIMIT &&
                    bc <= LIMIT)
                {
                    printf(
                        "3-WELL GROUP FOUND:\n"
                        "  #%d (%d,%d)\n"
                        "  #%d (%d,%d)\n"
                        "  #%d (%d,%d)\n"
                        "  distances: %.2f %.2f %.2f\n\n",
                        a + 1, wells[a].bx, wells[a].bz,
                        b + 1, wells[b].bx, wells[b].bz,
                        c + 1, wells[c].bx, wells[c].bz,
                        ab, ac, bc
                    );

                    found3 = 1;
                }
            }
        }
    }

    if (!found3)
        printf("No physical 3-well group found.\n");

    /*
     * Physical 4-well test.
     */
    printf("\nPHYSICAL 4-WELL TEST\n");
    printf("----------------------------------------\n");

    int found4 = 0;

    for (int a = 0; a < count; a++)
    {
        for (int b = a + 1; b < count; b++)
        {
            for (int c = b + 1; c < count; c++)
            {
                for (int d = c + 1; d < count; d++)
                {
                    const double LIMIT = 32.0;

                    double ab = sqrt(dist2(&wells[a], &wells[b]));
                    double ac = sqrt(dist2(&wells[a], &wells[c]));
                    double ad = sqrt(dist2(&wells[a], &wells[d]));
                    double bc = sqrt(dist2(&wells[b], &wells[c]));
                    double bd = sqrt(dist2(&wells[b], &wells[d]));
                    double cd = sqrt(dist2(&wells[c], &wells[d]));

                    if (ab <= LIMIT &&
                        ac <= LIMIT &&
                        ad <= LIMIT &&
                        bc <= LIMIT &&
                        bd <= LIMIT &&
                        cd <= LIMIT)
                    {
                        printf(
                            "4-WELL GROUP FOUND:\n"
                            "  #%d (%d,%d)\n"
                            "  #%d (%d,%d)\n"
                            "  #%d (%d,%d)\n"
                            "  #%d (%d,%d)\n"
                            "  distances:\n"
                            "    %.2f %.2f %.2f\n"
                            "    %.2f %.2f %.2f\n\n",
                            a + 1, wells[a].bx, wells[a].bz,
                            b + 1, wells[b].bx, wells[b].bz,
                            c + 1, wells[c].bx, wells[c].bz,
                            d + 1, wells[d].bx, wells[d].bz,
                            ab, ac, ad, bc, bd, cd
                        );

                        found4 = 1;
                    }
                }
            }
        }
    }

    if (!found4)
        printf("No physical 4-well group found.\n");

    printf("\n========================================\n");
    printf("VALIDATION COMPLETE\n");
    printf("3-well : %s\n", found3 ? "FOUND" : "NOT FOUND");
    printf("4-well : %s\n", found4 ? "FOUND" : "NOT FOUND");
    printf("========================================\n");

    return 0;
}
