#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "finders.h"
#include "generator.h"

typedef struct
{
    int cx;
    int cz;
    int x;
    int z;
} Well;

typedef struct
{
    Well w[4];
    int count;
    int minX;
    int minZ;
    int maxX;
    int maxZ;
} Cluster;


/*
 * ------------------------------------------------------------
 * Validate ONE actual Desert Well.
 *
 * This is deliberately two-stage:
 *
 *   1. getStructurePos()
 *      -> decoration RNG
 *
 *   2. isViableStructurePos()
 *      -> biome validation
 *
 * Therefore an accepted well is not RNG-only.
 * ------------------------------------------------------------
 */
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
    {
        return 0;
    }

    if (!isViableStructurePos(
            Desert_Well,
            g,
            p.x,
            p.z,
            0))
    {
        return 0;
    }

    if (out)
    {
        out->cx = cx;
        out->cz = cz;
        out->x = p.x;
        out->z = p.z;
    }

    return 1;
}


/*
 * ------------------------------------------------------------
 * Check whether a chunk is already inside a cluster.
 * ------------------------------------------------------------
 */
static int cluster_has_chunk(
    const Cluster *c,
    int cx,
    int cz)
{
    for (int i = 0; i < c->count; i++)
    {
        if (c->w[i].cx == cx &&
            c->w[i].cz == cz)
        {
            return 1;
        }
    }

    return 0;
}


/*
 * ------------------------------------------------------------
 * Sort wells by chunk coordinate.
 *
 * This gives us deterministic output and makes duplicate
 * suppression easier.
 * ------------------------------------------------------------
 */
static void sort_wells(
    Well *w,
    int n)
{
    for (int i = 0; i < n; i++)
    {
        for (int j = i + 1; j < n; j++)
        {
            if (w[j].cx < w[i].cx ||
                (w[j].cx == w[i].cx &&
                 w[j].cz < w[i].cz))
            {
                Well t = w[i];
                w[i] = w[j];
                w[j] = t;
            }
        }
    }
}


/*
 * ------------------------------------------------------------
 * Print a validated cluster.
 * ------------------------------------------------------------
 */
static void print_cluster(
    uint64_t seed,
    const Cluster *c,
    int pattern)
{
    printf("\n========================================\n");

    if (pattern == 4)
        printf("VALID QUADWELL FOUND\n");
    else if (pattern == 3)
        printf("VALID 3-WELL CLUSTER FOUND\n");
    else
        printf("VALID 2-WELL CLUSTER FOUND\n");

    /*
     * Save accepted 3/4-well results immediately.
     * This prevents terminal/progress output from hiding results.
     */
    if (pattern == 3 || pattern == 4)
    {
        FILE *rf = fopen("well_results.txt", "a");

        if (rf)
        {
            fprintf(rf, "\n========================================\n");

            if (pattern == 4)
                fprintf(rf, "VALID PHYSICAL QUADWELL FOUND\n");
            else
                fprintf(rf, "VALID PHYSICAL 3-WELL CLUSTER FOUND\n");

            fprintf(rf, "Seed : %llu\n",
                (unsigned long long)seed);

            fprintf(rf, "----------------------------------------\n");

            for (int i = 0; i < c->count; i++)
            {
                const Well *w = &c->w[i];

                fprintf(
                    rf,
                    "Well %d\n"
                    "  Chunk : (%d, %d)\n"
                    "  World : (%d, %d)\n"
                    "  Local : (%d, %d)\n",
                    i + 1,
                    w->cx,
                    w->cz,
                    w->x,
                    w->z,
                    w->x & 15,
                    w->z & 15);
            }

            fprintf(rf, "========================================\n");
            fclose(rf);
        }
    }

    printf("Seed : %llu\n",
        (unsigned long long)seed);

    printf("----------------------------------------\n");

    for (int i = 0; i < c->count; i++)
    {
        const Well *w = &c->w[i];

        printf(
            "Well %d\n"
            "  Chunk : (%d, %d)\n"
            "  World : (%d, %d)\n"
            "  Local : (%d, %d)\n",
            i + 1,
            w->cx,
            w->cz,
            w->x,
            w->z,
            w->x & 15,
            w->z & 15);
    }

    printf("----------------------------------------\n");

    printf(
        "Chunk bounds: (%d,%d) -> (%d,%d)\n",
        c->minX,
        c->minZ,
        c->maxX,
        c->maxZ);

    printf("========================================\n");
}



/*
 * ------------------------------------------------------------
 * Physical 5x5 footprint connectivity.
 *
 * Each Desert Well footprint occupies:
 *
 *     X = anchorX .. anchorX+4
 *     Z = anchorZ .. anchorZ+4
 *
 * Two footprints are physically connected when their
 * 5x5 occupied areas touch or overlap.
 * ------------------------------------------------------------
 */
static int wells_physically_touch(
    const Well *a,
    const Well *b)
{
    int ax1 = a->x;
    int az1 = a->z;
    int ax2 = b->x;
    int az2 = b->z;

    int amaxX = ax1 + 4;
    int amaxZ = az1 + 4;

    int bmaxX = ax2 + 4;
    int bmaxZ = az2 + 4;

    int xgap = 0;
    int zgap = 0;

    if (amaxX < ax2)
        xgap = ax2 - amaxX - 1;
    else if (bmaxX < ax1)
        xgap = ax1 - bmaxX - 1;

    if (amaxZ < az2)
        zgap = az2 - amaxZ - 1;
    else if (bmaxZ < az1)
        zgap = az1 - bmaxZ - 1;

    return xgap == 0 && zgap == 0;
}


/*
 * Check whether all wells in a cluster form one physically
 * connected group using their actual 5x5 footprints.
 */
static int cluster_physically_connected(
    const Cluster *c)
{
    if (c->count <= 1)
        return 1;

    int connected[4] = {0, 0, 0, 0};
    int changed = 1;

    connected[0] = 1;

    while (changed)
    {
        changed = 0;

        for (int i = 0; i < c->count; i++)
        {
            if (!connected[i])
                continue;

            for (int j = 0; j < c->count; j++)
            {
                if (connected[j])
                    continue;

                if (wells_physically_touch(
                        &c->w[i],
                        &c->w[j]))
                {
                    connected[j] = 1;
                    changed = 1;
                }
            }
        }
    }

    for (int i = 0; i < c->count; i++)
    {
        if (!connected[i])
            return 0;
    }

    return 1;
}


/*
 * ------------------------------------------------------------
 * 4-well:

 *
 * Exactly all four chunks of:
 *
 *   (x,   z)
 *   (x+1, z)
 *   (x,   z+1)
 *   (x+1, z+1)
 *
 * must contain valid Desert Wells.
 * ------------------------------------------------------------
 */
static int find_quadwell(
    Well *grid,
    int width,
    int height,
    int x,
    int z,
    Cluster *out)
{
    if (x < 0 || z < 0)
        return 0;

    if (x + 1 >= width ||
        z + 1 >= height)
        return 0;

    Well *a = &grid[z * width + x];
    Well *b = &grid[z * width + x + 1];
    Well *c = &grid[(z + 1) * width + x];
    Well *d = &grid[(z + 1) * width + x + 1];

    if (a->cx < 0 ||
        b->cx < 0 ||
        c->cx < 0 ||
        d->cx < 0)
    {
        return 0;
    }

    out->count = 4;

    out->w[0] = *a;
    out->w[1] = *b;
    out->w[2] = *c;
    out->w[3] = *d;

    sort_wells(out->w, 4);

    out->minX = out->w[0].cx;
    out->minZ = out->w[0].cz;
    out->maxX = out->w[0].cx;
    out->maxZ = out->w[0].cz;

    for (int i = 1; i < 4; i++)
    {
        if (out->w[i].cx < out->minX)
            out->minX = out->w[i].cx;

        if (out->w[i].cz < out->minZ)
            out->minZ = out->w[i].cz;

        if (out->w[i].cx > out->maxX)
            out->maxX = out->w[i].cx;

        if (out->w[i].cz > out->maxZ)
            out->maxZ = out->w[i].cz;
    }

    /*
     * ------------------------------------------------------------
     * QUADWELL FOOTPRINT DIAGNOSTIC
     *
     * Diagnostic only.
     * Does NOT affect quadwell detection.
     * ------------------------------------------------------------
     */
    printf("\n[QUADWELL FOOTPRINT DIAGNOSTIC]\n");

    for (int i = 0; i < 4; i++)
    {
        for (int j = i + 1; j < 4; j++)
        {
            int dx = out->w[j].x - out->w[i].x;
            int dz = out->w[j].z - out->w[i].z;

            int adx = dx < 0 ? -dx : dx;
            int adz = dz < 0 ? -dz : dz;

            int chebyshev = adx > adz ? adx : adz;
            int manhattan = adx + adz;

            printf(
                "W%d-W%d: dx=%d dz=%d | "
                "abs=(%d,%d) | "
                "Chebyshev=%d | "
                "Manhattan=%d\n",
                i + 1,
                j + 1,
                dx,
                dz,
                adx,
                adz,
                chebyshev,
                manhattan);
        }
    }

    return 1;
}



/*
 * ------------------------------------------------------------
 * Physical 3-well connectivity.
 *
 * A 3-well cluster is accepted only when all three actual
 * 5x5 Desert Well footprints form one connected group.
 * ------------------------------------------------------------
 */
static int threewell_physically_connected(
    const Cluster *c)
{
    if (c->count != 3)
        return 0;

    int connected[4] = {0, 0, 0, 0};
    int changed = 1;

    connected[0] = 1;

    while (changed)
    {
        changed = 0;

        for (int i = 0; i < c->count; i++)
        {
            if (!connected[i])
                continue;

            for (int j = 0; j < c->count; j++)
            {
                if (connected[j])
                    continue;

                if (wells_physically_touch(
                        &c->w[i],
                        &c->w[j]))
                {
                    connected[j] = 1;
                    changed = 1;
                }
            }
        }
    }

    for (int i = 0; i < c->count; i++)
    {
        if (!connected[i])
            return 0;
    }

    return 1;
}


/*
 * ------------------------------------------------------------
 * 3-well:

 *
 * Any 3 of the 4 positions inside the same 2x2 chunk block.
 * ------------------------------------------------------------
 */
static int find_threewell(
    Well *grid,
    int width,
    int height,
    int x,
    int z,
    Cluster *out)
{
    if (x < 0 || z < 0)
        return 0;

    if (x + 1 >= width ||
        z + 1 >= height)
        return 0;

    Well *p[4];

    p[0] = &grid[z * width + x];
    p[1] = &grid[z * width + x + 1];
    p[2] = &grid[(z + 1) * width + x];
    p[3] = &grid[(z + 1) * width + x + 1];

    int n = 0;

    for (int i = 0; i < 4; i++)
    {
        if (p[i]->cx >= 0)
        {
            out->w[n++] = *p[i];
        }
    }

    /*
     * Exactly 3 wells only.
     * A 4-well block is a quadwell and is NOT a 3-well result.
     */
    if (n != 3)
        return 0;

    out->count = 3;

    sort_wells(out->w, 3);

    out->minX = out->w[0].cx;
    out->minZ = out->w[0].cz;
    out->maxX = out->w[2].cx;
    out->maxZ = out->w[2].cz;

    return 1;
}


/*
 * ------------------------------------------------------------
 * 2-well:
 *
 * Any two valid wells whose chunk distance is <= 1 on both
 * axes, excluding the same chunk.
 *
 * This includes:
 *
 *   horizontal
 *   vertical
 *   diagonal
 *
 * neighboring chunks.
 * ------------------------------------------------------------
 */
static int find_twowell(
    Well *grid,
    int width,
    int height,
    int x,
    int z,
    Cluster *out)
{
    if (x < 0 || z < 0)
        return 0;

    Well *a = &grid[z * width + x];

    if (a->cx < 0)
        return 0;

    for (int dz = -1; dz <= 1; dz++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            if (dx == 0 && dz == 0)
                continue;

            int nx = x + dx;
            int nz = z + dz;

            if (nx < 0 || nz < 0 ||
                nx >= width || nz >= height)
                continue;

            Well *b = &grid[nz * width + nx];

            if (b->cx < 0)
                continue;

            out->count = 2;
            out->w[0] = *a;
            out->w[1] = *b;

            sort_wells(out->w, 2);

            out->minX = out->w[0].cx;
            out->minZ = out->w[0].cz;
            out->maxX = out->w[1].cx;
            out->maxZ = out->w[1].cz;

            return 1;
        }
    }

    return 0;
}


/*
 * ------------------------------------------------------------
 * Main search
 * ------------------------------------------------------------
 */
int main(int argc, char **argv)
{
    if (argc != 7)
    {
        printf(
            "Usage:\n"
            "  %s <seed_start> <seed_end> "
            "<centerX> <centerZ> <radius>\n\n"
            "Example:\n"
            "  %s 1520815389707 1520815389707 "
            "676297 -479950 1\n",
            argv[0],
            argv[0]);

        return 1;
    }

    uint64_t seedStart =
        strtoull(argv[1], NULL, 10);

    uint64_t seedEnd =
        strtoull(argv[2], NULL, 10);

    int centerX = atoi(argv[3]);
    int centerZ = atoi(argv[4]);
    int radius = atoi(argv[5]);
    int pattern = atoi(argv[6]);

    if (pattern != 0 && pattern != 2 &&
        pattern != 3 && pattern != 4)
    {
        fprintf(stderr, "Pattern must be 0, 2, 3, or 4\\n");
        return 1;
    }

    if (radius < 1)
    {
        fprintf(stderr,
            "Radius must be >= 1\n");
        return 1;
    }

    int width = radius * 2 + 1;
    int height = radius * 2 + 1;

    size_t total =
        (size_t)width * (size_t)height;

    uint64_t seedsScanned = 0;
    uint64_t validWells = 0;
    uint64_t quadwellFound = 0;
    uint64_t threewellSeedsFound = 0;

    Well *grid =
        malloc(total * sizeof(Well));

    if (!grid)
    {
        fprintf(stderr,
            "Memory allocation failed\n");
        return 1;
    }

    Generator g;

    setupGenerator(
        &g,
        MC_26_40,
        0);

    printf("========================================\n");
    printf(" Validated Desert Well Seed Search\n");
    printf("========================================\n");
    printf("Seed range : %llu -> %llu\n",
        (unsigned long long)seedStart,
        (unsigned long long)seedEnd);

    printf("Center     : (%d, %d)\n",
        centerX,
        centerZ);

    printf("Radius     : %d chunks\n",
        radius);

    printf("Area       : %d x %d chunks\n",
        width,
        height);

    printf("Validation : RNG + Desert biome\n");
    printf("========================================\n");

    time_t searchStartTime = time(NULL);
    const uint64_t progressStep = 1000000ULL;
    uint64_t nextProgress =
        ((seedStart / progressStep) + 1) * progressStep;

    for (uint64_t seed = seedStart;
         seed <= seedEnd;
         seed++)
    {
        seedsScanned++;

        applySeed(
            &g,
            DIM_OVERWORLD,
            seed);

        for (size_t i = 0;
             i < total;
             i++)
        {
            grid[i].cx = -1;
            grid[i].cz = -1;
            grid[i].x = 0;
            grid[i].z = 0;
        }

        int validCount = 0;
        int currentSeedHasThreewell = 0;

        /*
         * ----------------------------------------------------
         * Phase 1:
         *
         * Find every validated Desert Well in the region.
         * ----------------------------------------------------
         */
        for (int cx = centerX - radius;
             cx <= centerX + radius;
             cx++)
        {
            for (int cz = centerZ - radius;
                 cz <= centerZ + radius;
                 cz++)
            {
                int gx = cx - (centerX - radius);
                int gz = cz - (centerZ - radius);

                Well *slot =
                    &grid[gz * width + gx];

                if (get_valid_well(
                        &g,
                        seed,
                        cx,
                        cz,
                        slot))
                {
                    validCount++;
                    validWells++;
                }
            }
        }

        /*
         * ----------------------------------------------------
         * No wells -> nothing to cluster.
         * ----------------------------------------------------
         */
        if (validCount == 0)
            goto next_seed;

        /*
         * ----------------------------------------------------
         * Phase 2:
         *
         * Quadwell first.
         *
         * This is the strongest pattern, so test it first.
         * ----------------------------------------------------
         */
        if (pattern == 0 || pattern == 4)
        {
            for (int z = 0;
                 z + 1 < height;
                 z++)
            {
                for (int x = 0;
                     x + 1 < width;
                     x++)
                {
                    Cluster c;
    
                    if (find_quadwell(
                            grid,
                            width,
                            height,
                            x,
                            z,
                            &c))
                    {
                        if (!cluster_physically_connected(&c))
                            continue;

                        quadwellFound++;

                        print_cluster(
                            seed,
                            &c,
                            4);
                    }
                }
            }
        }

        /*
         * ----------------------------------------------------
         * Phase 3:
         *
         * 3 wells inside a 2x2 block.
         * ----------------------------------------------------
         */
        if (pattern == 0 || pattern == 3)
        {
            for (int z = 0;
                 z + 1 < height;
                 z++)
            {
                for (int x = 0;
                     x + 1 < width;
                     x++)
                {
                    Cluster c;
    
                    if (find_threewell(
                            grid,
                            width,
                            height,
                            x,
                            z,
                            &c))
                    {
                        if (!threewell_physically_connected(&c))
                            continue;

                        currentSeedHasThreewell = 1;

                        print_cluster(
                            seed,
                            &c,
                            3);
                    }
                }
            }
        }

        /*
         * ----------------------------------------------------
         * 3-WELL SEED LIMIT
         *
         * Count each seed only once even if it contains
         * multiple 3-well patterns.
         * Stop after 5 unique seeds.
         * ----------------------------------------------------
         */
        if (currentSeedHasThreewell)
        {
            threewellSeedsFound++;

            if (threewellSeedsFound >= 5)
                goto search_done;
        }

        /*
         * ----------------------------------------------------
         * Phase 4:
         *
         * Neighboring 2-well clusters.
         *
         * We only scan each anchor once, but duplicates can
         * still occur in different anchors. The final output
         * remains deterministic because wells are sorted.
         * ----------------------------------------------------
         */
        if (0)
        {
            for (int z = 0;
                 z < height;
                 z++)
            {
                for (int x = 0;
                     x < width;
                     x++)
                {
                    Cluster c;
    
                    if (find_twowell(
                            grid,
                            width,
                            height,
                            x,
                            z,
                            &c))
                    {
                        /*
                         * Physical 2-well validation.
                         *
                         * The two actual 5x5 well footprints must
                         * physically touch or overlap.
                         */
                        if (!wells_physically_touch(
                                &c.w[0],
                                &c.w[1]))
                            continue;

                        /*
                         * Print only when the first well is the
                         * current anchor. This suppresses the
                         * reverse duplicate.
                         */
                        if (c.w[0].cx ==
                                centerX - radius + x &&
                            c.w[0].cz ==
                                centerZ - radius + z)
                        {
                            print_cluster(
                                seed,
                                &c,
                                2);
                        }
                    }
                }
            }
        }

        /*
         * ----------------------------------------------------
         * Live progress display
         * ----------------------------------------------------
         * Update approximately once per second.
         */
        {
            static time_t lastDisplay = 0;
            time_t now = time(NULL);

            if (now != lastDisplay ||
                seed == seedEnd)
            {
                lastDisplay = now;

                double elapsed =
                    difftime(now, searchStartTime);

                double rate = 0.0;

                if (elapsed > 0.0)
                    rate =
                        (double)seedsScanned / elapsed;

                double percent = 0.0;

                if (seedEnd >= seedStart)
                {
                    percent =
                        ((double)(seed - seedStart + 1) /
                         (double)(seedEnd - seedStart + 1))
                        * 100.0;
                }

                printf(
                    "\rProgress: %.2f%% | "
                    "Seed: %llu / %llu | "
                    "3-well: %llu | "
                    "Rate: %.0f seeds/s | "
                    "Elapsed: %.0fs",
                    percent,
                    (unsigned long long)seed,
                    (unsigned long long)seedEnd,
                    (unsigned long long)threewellSeedsFound,
                    rate,
                    elapsed);

                fflush(stdout);
            }
        }

next_seed:

        if (seed == UINT64_MAX)
            break;
    }



search_done:

    printf("\n========================================\n");
    printf(" Search complete\n");
    printf("========================================\n");

    printf("Seeds scanned    : %llu\n",
        (unsigned long long)seedsScanned);
    printf("Valid wells      : %llu\n",
        (unsigned long long)validWells);
    printf("Quadwells found  : %llu\n",
        (unsigned long long)quadwellFound);
    printf("3-well seeds     : %llu\n",
        (unsigned long long)threewellSeedsFound);
    printf("========================================\n");

    free(grid);

    return 0;
}
