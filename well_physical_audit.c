#include <stdio.h>
#include <stdint.h>
#include "finders.h"
#include "generator.h"

typedef struct {
    int cx, cz;
    int x, z;
} Well;

static int get_valid_well(
    Generator *g,
    uint64_t seed,
    int cx,
    int cz,
    Well *out)
{
    Pos p;

    if (!getStructurePos(
            Desert_Well,
            MC_26_40,
            seed,
            cx,
            cz,
            &p))
        return 0;

    if (!isViableStructurePos(
            Desert_Well,
            g,
            p.x,
            p.z,
            0))
        return 0;

    out->cx = cx;
    out->cz = cz;
    out->x = p.x;
    out->z = p.z;

    return 1;
}

static int physical_touch(const Well *a, const Well *b)
{
    int ax1 = a->x;
    int az1 = a->z;
    int ax2 = a->x + 4;
    int az2 = a->z + 4;

    int bx1 = b->x;
    int bz1 = b->z;
    int bx2 = b->x + 4;
    int bz2 = b->z + 4;

    int xgap = 0;
    int zgap = 0;

    if (ax2 < bx1)
        xgap = bx1 - ax2 - 1;
    else if (bx2 < ax1)
        xgap = ax1 - bx2 - 1;

    if (az2 < bz1)
        zgap = bz1 - az2 - 1;
    else if (bz2 < az1)
        zgap = az1 - bz2 - 1;

    return xgap == 0 && zgap == 0;
}

int main(void)
{
    uint64_t seed = 1520815389707ULL;

    int centerX = 676297;
    int centerZ = -479950;

    Generator g;
    setupGenerator(&g, MC_26_40, 0);
    applySeed(&g, DIM_OVERWORLD, seed);

    Well wells[16];
    int count = 0;

    printf("========================================\n");
    printf(" Bedrock 26.40 Physical Well Audit\n");
    printf("========================================\n");
    printf("Seed   : %llu\n",
        (unsigned long long)seed);
    printf("Center : (%d, %d)\n",
        centerX, centerZ);
    printf("Area   : 3 x 3 chunks\n");
    printf("----------------------------------------\n");

    for (int cz = centerZ - 1; cz <= centerZ + 1; cz++)
    {
        for (int cx = centerX - 1; cx <= centerX + 1; cx++)
        {
            Well w;

            if (get_valid_well(
                    &g,
                    seed,
                    cx,
                    cz,
                    &w))
            {
                wells[count++] = w;

                printf(
                    "WELL %d\n"
                    "  Chunk : (%d, %d)\n"
                    "  World : (%d, %d)\n"
                    "  Local : (%d, %d)\n"
                    "  Box   : X=%d..%d Z=%d..%d\n\n",
                    count,
                    w.cx,
                    w.cz,
                    w.x,
                    w.z,
                    w.x & 15,
                    w.z & 15,
                    w.x,
                    w.x + 4,
                    w.z,
                    w.z + 4);
            }
        }
    }

    printf("----------------------------------------\n");
    printf("Valid wells: %d\n", count);

    if (count < 2)
    {
        printf("Not enough wells for pair analysis.\n");
        return 0;
    }

    printf("\n========================================\n");
    printf(" PAIRWISE PHYSICAL CONNECTIVITY\n");
    printf("========================================\n");

    int edges = 0;

    for (int i = 0; i < count; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            int dx = wells[j].x - wells[i].x;
            int dz = wells[j].z - wells[i].z;

            int adx = dx < 0 ? -dx : dx;
            int adz = dz < 0 ? -dz : dz;

            int touch = physical_touch(
                &wells[i],
                &wells[j]);

            printf(
                "W%d-W%d: dx=%d dz=%d "
                "abs=(%d,%d) "
                "TOUCH=%s\n",
                i + 1,
                j + 1,
                dx,
                dz,
                adx,
                adz,
                touch ? "YES" : "NO");

            if (touch)
                edges++;
        }
    }

    printf("\n----------------------------------------\n");
    printf("Physical connections: %d\n", edges);

    /*
     * Simple connected-component test.
     */
    int connected[16] = {0};
    int changed = 1;

    connected[0] = 1;

    while (changed)
    {
        changed = 0;

        for (int i = 0; i < count; i++)
        {
            if (!connected[i])
                continue;

            for (int j = 0; j < count; j++)
            {
                if (connected[j])
                    continue;

                if (physical_touch(
                        &wells[i],
                        &wells[j]))
                {
                    connected[j] = 1;
                    changed = 1;
                }
            }
        }
    }

    int allConnected = 1;

    for (int i = 0; i < count; i++)
    {
        if (!connected[i])
            allConnected = 0;
    }

    printf("\n========================================\n");
    printf(" CONNECTIVITY VERDICT\n");
    printf("========================================\n");

    if (allConnected)
        printf("ALL %d WELLS FORM ONE PHYSICAL CLUSTER\n",
            count);
    else
        printf("WELLS ARE NOT ONE PHYSICAL CLUSTER\n");

    return 0;
}
