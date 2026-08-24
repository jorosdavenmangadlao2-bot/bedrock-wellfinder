#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "finders.h"
#include "generator.h"

#define NUM_THREADS 4

/*
 * ------------------------------------------------------------
 * Stage 4B: Random signed 64-bit seed support
 *
 * Uses /dev/urandom when available so both positive and
 * negative Minecraft seeds can be selected uniformly.
 *
 * No Desert Well logic is changed here.
 * ------------------------------------------------------------
 */
static int random_seed64(int64_t *out)
{
    FILE *fp = fopen("/dev/urandom", "rb");

    if (fp)
    {
        uint64_t bits;

        if (fread(&bits, sizeof(bits), 1, fp) == 1)
        {
            fclose(fp);
            *out = (int64_t)bits;
            return 1;
        }

        fclose(fp);
    }

    /*
     * Fallback: combine several standard rand() calls.
     * This is only a fallback; /dev/urandom is preferred.
     */
    uint64_t bits = 0;

    bits |= ((uint64_t)(unsigned int)rand()) << 32;
    bits |= (uint64_t)(unsigned int)rand();

    *out = (int64_t)bits;
    return 1;
}

/* ------------------------------------------------------------
 * Stage 4B-3: Reproducible random signed 64-bit seed generator
 * ------------------------------------------------------------ */

static uint64_t rng64_next(uint64_t *state)
{
    uint64_t x = *state;

    /* SplitMix64 */
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;

    *state = x;
    return x;
}

static int64_t rng64_signed(uint64_t *state)
{
    return (int64_t)rng64_next(state);
}
static int64_t rng64_range(uint64_t *state, int64_t min, int64_t max)
{
    uint64_t span = (uint64_t)(max - min) + 1ULL;
    uint64_t r = rng64_next(state);

    return min + (int64_t)(r % span);
}

static int parse_seed(const char *s, uint64_t *out)
{
    char *end = NULL;

    errno = 0;
    int64_t v = strtoll(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0')
        return 0;

    *out = (uint64_t)v;
    return 1;
}

static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

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

    int sampleX = p.x >> 2;
    int sampleZ = p.z >> 2;

    int biomeID = getBiomeAt(
        g,
        0,
        sampleX,
        319 >> 2,
        sampleZ
    );

    if (biomeID != desert)
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
    int dx = abs(a->x - b->x);
    int dz = abs(a->z - b->z);

    /*
     * Each well occupies a 5x5 footprint:
     *
     *     X = anchorX .. anchorX+4
     *     Z = anchorZ .. anchorZ+4
     *
     * Physical connection:
     *
     *   dx <= 5 && dz <= 4
     *       OR
     *   dx <= 4 && dz <= 5
     *
     * Corner-only contact (5,5) is NOT connected.
     */
    return (dx <= 5 && dz <= 4) ||
           (dx <= 4 && dz <= 5);
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
/*
 * PHYSICAL WELL COMPONENT SCANNER
 * Finds connected groups using actual 5x5 footprints.
 */
static int scan_physical_components(
    Well *grid, int width, int height, Cluster *clusters, int maxClusters)
{
    int total = width * height;
    int component_count = 0;

    int *visited = malloc((size_t)total * sizeof(int));
    int *queue = malloc((size_t)total * sizeof(int));

    if (!visited || !queue)
    {
        fprintf(stderr,
                "Physical component memory allocation failed "
                "(%d cells)\n", total);

        free(visited);
        free(queue);
        return 0;
    }

    for (int i = 0; i < total; i++)
        visited[i] = 0;

    for (int i = 0; i < total; i++)
    {
        if (visited[i] || grid[i].cx < 0)
            continue;

        if (component_count >= maxClusters)
            break;

        int qhead = 0;
        int qtail = 0;

        queue[qtail++] = i;
        visited[i] = 1;

        Cluster *c = &clusters[component_count];
        c->count = 0;

        while (qhead < qtail)
        {
            int cur = queue[qhead++];

            /*
             * Keep the REAL component count.
             * Cluster can store only four Well objects,
             * but count must not be truncated.
             */
            if (c->count < 4)
                c->w[c->count] = grid[cur];

            c->count++;

            int cx = cur % width;
            int cz = cur / width;

            for (int dz = -1; dz <= 1; dz++)
            {
                for (int dx = -1; dx <= 1; dx++)
                {
                    if (dx == 0 && dz == 0)
                        continue;

                    int nx = cx + dx;
                    int nz = cz + dz;

                    if (nx < 0 || nz < 0 ||
                        nx >= width || nz >= height)
                        continue;

                    int ni = nz * width + nx;

                    if (visited[ni] || grid[ni].cx < 0)
                        continue;

                    if (wells_physically_touch(
                            &grid[cur], &grid[ni]))
                    {
                        visited[ni] = 1;
                        queue[qtail++] = ni;
                    }
                }
            }
        }

        /*
         * Only build Cluster geometry when the component
         * fits inside the Cluster storage.
         */
        if (c->count >= 1 && c->count <= 4)
        {
            sort_wells(c->w, c->count);

            c->minX = c->maxX = c->w[0].cx;
            c->minZ = c->maxZ = c->w[0].cz;

            for (int j = 1; j < c->count; j++)
            {
                if (c->w[j].cx < c->minX) c->minX = c->w[j].cx;
                if (c->w[j].cx > c->maxX) c->maxX = c->w[j].cx;
                if (c->w[j].cz < c->minZ) c->minZ = c->w[j].cz;
                if (c->w[j].cz > c->maxZ) c->maxZ = c->w[j].cz;
            }
        }

        component_count++;
    }

    free(visited);
    free(queue);

    return component_count;
}


typedef struct {
    int64_t seedStart;
    int64_t seedEnd;
    int centerX;
    int centerZ;
    int radius;
    int pattern;
} ThreadArgs;

typedef struct {
    int64_t seed;
    Cluster cluster;
} PhysicalResult;

typedef struct {
    uint64_t seedsScanned;
    uint64_t validWells;
    uint64_t quadwellFound;
    uint64_t threewellSeedsFound;
    uint64_t physical3Found;
    uint64_t physical4Found;

    PhysicalResult *physicalResults;
    size_t physicalResultCount;
    size_t physicalResultCapacity;
} ThreadResult;


/*
 * ------------------------------------------------------------
 * Stage 4B-2: Random signed 64-bit seed worker
 *
 * Each worker receives a number of random seeds.
 * The existing proven worker() performs the actual
 * Desert Well / physical 3-well / physical 4-well scan.
 * ------------------------------------------------------------
 */

typedef struct
{
    int count;
    int64_t *seeds;
    int *centerXs;
    int *centerZs;

    int centerX;
    int centerZ;
    int radius;
    int pattern;
} RandomThreadArgs;

static void merge_thread_result(
    ThreadResult *dst,
    ThreadResult *src)
{
    if (!dst || !src)
        return;

    dst->seedsScanned += src->seedsScanned;
    dst->validWells += src->validWells;
    dst->quadwellFound += src->quadwellFound;
    dst->threewellSeedsFound += src->threewellSeedsFound;
    dst->physical3Found += src->physical3Found;
    dst->physical4Found += src->physical4Found;

    for (size_t i = 0; i < src->physicalResultCount; i++)
    {
        if (dst->physicalResultCount >= dst->physicalResultCapacity)
        {
            size_t newCapacity =
                dst->physicalResultCapacity * 2;

            PhysicalResult *newResults =
                realloc(
                    dst->physicalResults,
                    newCapacity * sizeof(PhysicalResult)
                );

            if (!newResults)
                break;

            dst->physicalResults = newResults;
            dst->physicalResultCapacity = newCapacity;
        }

        dst->physicalResults[
            dst->physicalResultCount++
        ] = src->physicalResults[i];
    }
}

static void *worker(void *arg);

static void *random_worker(void *arg)
{
    RandomThreadArgs *a =
        (RandomThreadArgs *)arg;

    ThreadResult *total =
        calloc(1, sizeof(ThreadResult));

    if (!total)
        return NULL;

    total->physicalResultCapacity = 16;
    total->physicalResults =
        malloc(
            total->physicalResultCapacity *
            sizeof(PhysicalResult)
        );

    if (!total->physicalResults)
    {
        free(total);
        return NULL;
    }

    for (int i = 0; i < a->count; i++)
    {
        int64_t randomSeed = a->seeds[i];

        ThreadArgs one;

        one.seedStart = randomSeed;
        one.seedEnd   = randomSeed;
        one.centerX   = a->centerXs[i];
        one.centerZ   = a->centerZs[i];
        one.radius    = a->radius;
        one.pattern   = a->pattern;

        ThreadResult *r =
            (ThreadResult *)worker(&one);

        if (!r)
            continue;

        merge_thread_result(total, r);

        free(r->physicalResults);
        free(r);
    }

    return total;
}

static int single_thread_main(int argc, char **argv);

static void *worker(void *arg)
{
    ThreadArgs *a = (ThreadArgs *)arg;

    ThreadResult *r = calloc(1, sizeof(ThreadResult));
    if (!r)
        return NULL;

    r->physicalResultCapacity = 16;
    r->physicalResults = malloc(
        r->physicalResultCapacity * sizeof(PhysicalResult)
    );

    if (!r->physicalResults)
    {
        free(r);
        return NULL;
    }

    int width = a->radius * 2 + 1;
    int height = a->radius * 2 + 1;

    size_t total = (size_t)width * (size_t)height;

    Well *grid = malloc(total * sizeof(Well));
    if (!grid)
    {
        free(r);
        return NULL;
    }

    Generator g;

    setupGenerator(
        &g,
        MC_26_40,
        0);

    for (int64_t seed = a->seedStart;
         seed <= a->seedEnd;
         seed++)
    {
        r->seedsScanned++;

        uint64_t mcSeed = (uint64_t)seed;

        applySeed(
            &g,
            DIM_OVERWORLD,
            mcSeed);

        for (size_t i = 0; i < total; i++)
        {
            grid[i].cx = -1;
            grid[i].cz = -1;
            grid[i].x = 0;
            grid[i].z = 0;
        }

        int validCount = 0;
        int currentSeedHasThreewell = 0;

        /*
         * Phase 1:
         * Find every validated Desert Well.
         */

        for (int cx = a->centerX - a->radius;
             cx <= a->centerX + a->radius;
             cx++)
        {
            for (int cz = a->centerZ - a->radius;
                 cz <= a->centerZ + a->radius;
                 cz++)
            {
                int gx =
                    cx - (a->centerX - a->radius);

                int gz =
                    cz - (a->centerZ - a->radius);

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
                    r->validWells++;

                    printf(
                        "DEBUG WELL: seed=%llu chunk=(%d,%d) world=(%d,%d) local=(%d,%d)\\n",
                        (unsigned long long)seed,
                        slot->cx,
                        slot->cz,
                        slot->x,
                        slot->z,
                        slot->x & 15,
                        slot->z & 15
                    );
                    fflush(stdout);
                }
            }
        }

        if (validCount == 0)
            goto next_seed;

        /*
         * Physical component scan.
         */

        {
            Cluster components[128];

            int componentCount =
                scan_physical_components(
                    grid,
                    width,
                    height,
                    components,
                    128);

            printf("\nDEBUG PHYSICAL COMPONENTS: %d\\n", componentCount);
            for (int di = 0; di < componentCount; di++) {
                printf("DEBUG COMPONENT %d: count=%d\\n",
                       di + 1, components[di].count);
                if (components[di].count > 0 &&
                    components[di].count <= 4) {
                    for (int wi = 0; wi < components[di].count; wi++) {
                        printf("  W%d chunk=(%d,%d) world=(%d,%d)\\n",
                               wi + 1,
                               components[di].w[wi].cx,
                               components[di].w[wi].cz,
                               components[di].w[wi].x,
                               components[di].w[wi].z);
                    }
                }
            }

            for (int ci = 0;
                 ci < componentCount;
                 ci++)
            {
                Cluster *pc =
                    &components[ci];

                if (pc->count != 3 &&
                    pc->count != 4)
                    continue;

                if (pc->count == 3)
                    r->physical3Found++;
                else
                    r->physical4Found++;

                /*
                 * Save the complete physical result.
                 * Grow the array when necessary.
                 */
                if (r->physicalResultCount >=
                    r->physicalResultCapacity)
                {
                    size_t newCapacity =
                        r->physicalResultCapacity * 2;

                    PhysicalResult *newResults =
                        realloc(
                            r->physicalResults,
                            newCapacity *
                            sizeof(PhysicalResult));

                    if (!newResults)
                    {
                        pthread_mutex_lock(
                            &output_mutex);

                        fprintf(
                            stderr,
                            "\nWARNING: Could not store "
                            "physical result for seed %lld\n",
                            (long long)seed);

                        pthread_mutex_unlock(
                            &output_mutex);
                    }
                    else
                    {
                        r->physicalResults = newResults;
                        r->physicalResultCapacity =
                            newCapacity;

                        r->physicalResults[
                            r->physicalResultCount
                        ].seed = seed;

                        r->physicalResults[
                            r->physicalResultCount
                        ].cluster = *pc;

                        r->physicalResultCount++;
                    }
                }
                else
                {
                    r->physicalResults[
                        r->physicalResultCount
                    ].seed = seed;

                    r->physicalResults[
                        r->physicalResultCount
                    ].cluster = *pc;

                    r->physicalResultCount++;
                }

                pthread_mutex_lock(
                    &output_mutex);

                printf(
                    "\n========================================\n");

                if (pc->count == 3)
                    printf(
                        "VALID PHYSICAL 3-WELL FOUND\n");
                else
                    printf(
                        "VALID PHYSICAL 4-WELL FOUND\n");

                printf(
                    "Seed : %llu\n",
                    (unsigned long long)seed);

                printf(
                    "----------------------------------------\n");

                for (int wi = 0;
                     wi < pc->count;
                     wi++)
                {
                    const Well *w =
                        &pc->w[wi];

                    printf(
                        "Well %d\n"
                        "  Chunk : (%d, %d)\n"
                        "  World : (%d, %d)\n"
                        "  Local : (%d, %d)\n",
                        wi + 1,
                        w->cx,
                        w->cz,
                        w->x,
                        w->z,
                        w->x & 15,
                        w->z & 15);
                }

                printf(
                    "----------------------------------------\n");

                printf(
                    "Chunk bounds: (%d,%d) -> (%d,%d)\n",
                    pc->minX,
                    pc->minZ,
                    pc->maxX,
                    pc->maxZ);

                printf(
                    "========================================\n");

                fflush(stdout);

                pthread_mutex_unlock(
                    &output_mutex);
            }
        }

        /*
         * Quadwell scan.
         */

        if (a->pattern == 0 ||
            a->pattern == 4)
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
                        if (!cluster_physically_connected(
                                &c))
                            continue;

                        r->quadwellFound++;

                        pthread_mutex_lock(
                            &output_mutex);

                        print_cluster(
                            seed,
                            &c,
                            4);

                        pthread_mutex_unlock(
                            &output_mutex);
                    }
                }
            }
        }

        /*
         * 3-well scan.
         */

        if (a->pattern == 0 ||
            a->pattern == 3)
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
                        if (!threewell_physically_connected(
                                &c))
                            continue;

                        currentSeedHasThreewell = 1;

                        pthread_mutex_lock(
                            &output_mutex);

                        print_cluster(
                            seed,
                            &c,
                            3);

                        pthread_mutex_unlock(
                            &output_mutex);
                    }
                }
            }
        }

        if (currentSeedHasThreewell)
            r->threewellSeedsFound++;

next_seed:

        if (seed == UINT64_MAX)
            break;
    }

    free(grid);

    return r;
}

static int single_thread_main(int argc, char **argv)
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

    /* Physical component results */
    uint64_t physical3Found = 0;
    uint64_t physical4Found = 0;

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
         * PHYSICAL COMPONENT SCAN
         *
         * Uses the actual 5x5 Desert Well footprints.
         *
         * No 2x2 chunk requirement.
         *
         * Exactly 3 connected wells:
         *     -> physical 3-well
         *
         * Exactly 4 connected wells:
         *     -> physical 4-well
         *
         * Components larger than 4 are rejected.
         * ----------------------------------------------------
         */
        {
            Cluster components[128];

            int componentCount =
                scan_physical_components(
                    grid,
                    width,
                    height,
                    components,
                    128);

            for (int ci = 0; ci < componentCount; ci++)
            {
                Cluster *pc = &components[ci];


                if (pc->count != 3 &&
                    pc->count != 4)
                    continue;

                if (pc->count == 3)
                    physical3Found++;
                else
                    physical4Found++;

                printf("\n========================================\n");

                if (pc->count == 3)
                    printf("VALID PHYSICAL 3-WELL FOUND\n");
                else
                    printf("VALID PHYSICAL 4-WELL FOUND\n");

                printf(
                    "Seed : %llu\n",
                    (unsigned long long)seed);

                printf("----------------------------------------\n");

                for (int wi = 0; wi < pc->count; wi++)
                {
                    const Well *w = &pc->w[wi];

                    printf(
                        "Well %d\n"
                        "  Chunk : (%d, %d)\n"
                        "  World : (%d, %d)\n"
                        "  Local : (%d, %d)\n",
                        wi + 1,
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
                    pc->minX,
                    pc->minZ,
                    pc->maxX,
                    pc->maxZ);

                printf("========================================\n");
            }
        }

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

    printf("Physical 3-wells : %llu\n",
        (unsigned long long)physical3Found);

    printf("Physical 4-wells : %llu\n",
        (unsigned long long)physical4Found);
    printf("========================================\n");

    free(grid);

    return 0;
}

int main(int argc, char **argv)
{
    /*
     * --------------------------------------------------------
     * Stage 4B-2 RANDOM MODE
     *
     * Usage:
     *   program random <count> <centerX> <centerZ> <radius> <pattern>
     *
     * Example:
     *   ./well_search_physical_finder random 10000 676297 -479950 2 0
     * --------------------------------------------------------
     */
    if (argc == 8 &&
        strcmp(argv[1], "random") == 0)
    {
        int randomCount = atoi(argv[2]);

        char *rngEnd = NULL;
        errno = 0;

        uint64_t rngSeed = strtoull(
            argv[3],
            &rngEnd,
            10
        );

        if (errno != 0 ||
            rngEnd == argv[3] ||
            *rngEnd != '\0')
        {
            fprintf(stderr,
                "Invalid RNG seed. Use an unsigned 64-bit integer.\n");
            return 1;
        }

        int centerX = atoi(argv[4]);
        int centerZ = atoi(argv[5]);
        int radius  = atoi(argv[6]);
        int pattern = atoi(argv[7]);

        if (randomCount <= 0)
        {
            fprintf(stderr,
                "Invalid random seed count.\n");
            return 1;
        }

        /*
         * Stage 4B-3:
         * Generate the complete signed 64-bit Minecraft seed list
         * before starting any worker threads.
         */
        int64_t *randomSeeds =
            malloc((size_t)randomCount * sizeof(int64_t));

        if (!randomSeeds)
        {
            fprintf(stderr,
                "Failed to allocate random seed list.\n");
            return 1;
        }

        uint64_t rngState = rngSeed;

        int64_t coordRange = 100000;

        int *randomCenterXs =
            malloc((size_t)randomCount * sizeof(int));

        int *randomCenterZs =
            malloc((size_t)randomCount * sizeof(int));

        if (!randomCenterXs || !randomCenterZs)
        {
            fprintf(stderr,
                "Failed to allocate random coordinate lists.\\n");
            free(randomCenterXs);
            free(randomCenterZs);
            free(randomSeeds);
            return 1;
        }

        for (int i = 0; i < randomCount; i++)
        {
            randomSeeds[i] = rng64_signed(&rngState);

            randomCenterXs[i] =
                (int)rng64_range(
                    &rngState,
                    (int64_t)centerX - coordRange,
                    (int64_t)centerX + coordRange
                );

            randomCenterZs[i] =
                (int)rng64_range(
                    &rngState,
                    (int64_t)centerZ - coordRange,
                    (int64_t)centerZ + coordRange
                );
        }

        /* Stage 4C-5:
         * Production mode: do not print every generated seed
         * or search coordinate. Keep only compact search metadata.
         */
        printf("Coordinate range : +/- %lld chunks\n",
            (long long)coordRange);
        printf("Base center      : (%d, %d)\n",
            centerX,
            centerZ);
        printf("========================================\n");

        pthread_t threads[NUM_THREADS];
        RandomThreadArgs args[NUM_THREADS];
        ThreadResult *results[NUM_THREADS] = {0};

        int base =
            randomCount / NUM_THREADS;

        int rem =
            randomCount % NUM_THREADS;

        printf("========================================\n");
        printf(" 4-THREAD RANDOM DESERT WELL SEARCH\n");
        printf("========================================\n");
        printf("Random seeds : %d\n", randomCount);
        printf("RNG seed     : %llu\n",
            (unsigned long long)rngSeed);
        printf("Threads      : %d\n", NUM_THREADS);
        printf("Center       : (%d, %d)\n",
            centerX, centerZ);
        printf("Radius       : %d\n", radius);
        printf("Pattern      : %d\n", pattern);
        printf("========================================\n");

        int seedOffset = 0;

        struct timespec searchStart, searchEnd;
        clock_gettime(CLOCK_MONOTONIC, &searchStart);

        for (int i = 0; i < NUM_THREADS; i++)
        {
            args[i].count =
                base + (i < rem ? 1 : 0);

            args[i].seeds =
                randomSeeds + seedOffset;

            args[i].centerXs =
                randomCenterXs + seedOffset;

            args[i].centerZs =
                randomCenterZs + seedOffset;

            seedOffset += args[i].count;

            args[i].centerX = centerX;
            args[i].centerZ = centerZ;
            args[i].radius  = radius;
            args[i].pattern = pattern;

            printf(
                "Thread %d : %d random seeds\n",
                i + 1,
                args[i].count
            );

            if (pthread_create(
                    &threads[i],
                    NULL,
                    random_worker,
                    &args[i]) != 0)
            {
                fprintf(
                    stderr,
                    "Failed to create random thread %d\n",
                    i + 1
                );

                return 1;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &searchEnd);

        double elapsed =
            (double)(searchEnd.tv_sec - searchStart.tv_sec) +
            (double)(searchEnd.tv_nsec - searchStart.tv_nsec) /
            1000000000.0;

        uint64_t seedsScanned = 0;
        uint64_t validWells = 0;
        uint64_t quadwellFound = 0;
        uint64_t threewellSeedsFound = 0;
        uint64_t physical3Found = 0;
        uint64_t physical4Found = 0;

        int resultNumber = 0;

        for (int i = 0; i < NUM_THREADS; i++)
        {
            void *ret = NULL;

            pthread_join(
                threads[i],
                &ret
            );

            results[i] =
                (ThreadResult *)ret;

            if (!results[i])
                continue;

            seedsScanned +=
                results[i]->seedsScanned;

            validWells +=
                results[i]->validWells;

            quadwellFound +=
                results[i]->quadwellFound;

            threewellSeedsFound +=
                results[i]->threewellSeedsFound;

            physical3Found +=
                results[i]->physical3Found;

            physical4Found +=
                results[i]->physical4Found;
        }

        printf("\n========================================\n");
        printf(" RANDOM SEARCH COMPLETE\n");
        printf("========================================\n");
        printf("Seeds scanned    : %llu\n",
            (unsigned long long)seedsScanned);
        printf("Valid wells      : %llu\n",
            (unsigned long long)validWells);
        printf("Quadwells found  : %llu\n",
            (unsigned long long)quadwellFound);
        printf("3-well seeds     : %llu\n",
            (unsigned long long)threewellSeedsFound);
        printf("Physical 3-wells : %llu\n",
            (unsigned long long)physical3Found);
        printf("Physical 4-wells : %llu\n",
            (unsigned long long)physical4Found);
        printf("Elapsed time     : %.3f seconds\n", elapsed);
        printf("========================================\n");

        printf("\n========================================\n");
        printf(" RANDOM PHYSICAL WELL RESULTS\n");
        printf("========================================\n");

        for (int i = 0; i < NUM_THREADS; i++)
        {
            if (!results[i])
                continue;

            for (size_t j = 0;
                 j < results[i]->physicalResultCount;
                 j++)
            {
                PhysicalResult *pr =
                    &results[i]->physicalResults[j];

                Cluster *c =
                    &pr->cluster;

                resultNumber++;

                printf(
                    "\n----------------------------------------\n"
                );

                if (c->count == 3)
                    printf(
                        "RANDOM PHYSICAL 3-WELL #%d\n",
                        resultNumber
                    );
                else if (c->count == 4)
                    printf(
                        "RANDOM PHYSICAL 4-WELL #%d\n",
                        resultNumber
                    );
                else
                    continue;

                printf(
                    "Seed : %lld\n",
                    (long long)pr->seed
                );

                printf(
                    "----------------------------------------\n"
                );

                for (int wi = 0;
                     wi < c->count;
                     wi++)
                {
                    const Well *w =
                        &c->w[wi];

                    printf(
                        "Well %d\n"
                        "  Chunk : (%d, %d)\n"
                        "  World : (%d, %d)\n"
                        "  Local : (%d, %d)\n",
                        wi + 1,
                        w->cx,
                        w->cz,
                        w->x,
                        w->z,
                        w->x & 15,
                        w->z & 15
                    );
                }

                printf(
                    "----------------------------------------\n"
                );

                printf(
                    "Chunk bounds: (%d,%d) -> (%d,%d)\n",
                    c->minX,
                    c->minZ,
                    c->maxX,
                    c->maxZ
                );
            }
        }

        if (resultNumber == 0)
        {
            printf(
                "No physical 3-well or 4-well "
                "clusters found.\n"
            );
        }
        else
        {
            printf(
                "\nTotal random physical results: %d\n",
                resultNumber
            );
        }

        for (int i = 0; i < NUM_THREADS; i++)
        {
            if (!results[i])
                continue;

            free(results[i]->physicalResults);
            free(results[i]);
        }

        free(randomCenterXs);
        free(randomCenterZs);
        free(randomSeeds);

        return 0;
    }

    /*
     * Existing deterministic signed-range mode.
     */
    if (argc != 7)
    {
        fprintf(stderr,
            "Usage: %s <seed_start> <seed_end> "
            "<centerX> <centerZ> <radius> <pattern>\n",
            argv[0]);
        return 1;
    }

    uint64_t seedStartBits;
    uint64_t seedEndBits;

    if (!parse_seed(argv[1], &seedStartBits) ||
        !parse_seed(argv[2], &seedEndBits))
    {
        fprintf(stderr, "Invalid seed. Use a signed 64-bit integer.\n");
        return 1;
    }

    int64_t seedStart = (int64_t)seedStartBits;
    int64_t seedEnd   = (int64_t)seedEndBits;

    int centerX = atoi(argv[3]);
    int centerZ = atoi(argv[4]);
    int radius  = atoi(argv[5]);
    int pattern = atoi(argv[6]);

    if (seedEnd < seedStart)
    {
        fprintf(stderr, "Invalid seed range.\n");
        return 1;
    }

    uint64_t total =
        (uint64_t)(seedEnd - seedStart) + 1ULL;

    uint64_t base =
        total / NUM_THREADS;

    uint64_t rem =
        total % NUM_THREADS;

    pthread_t threads[NUM_THREADS];
    ThreadArgs args[NUM_THREADS];
    ThreadResult *results[NUM_THREADS] = {0};

    int64_t current = seedStart;

    printf("========================================\n");
    printf(" 4-THREAD DESERT WELL SEARCH\n");
    printf("========================================\n");

    printf("Seed range : %lld -> %lld\n",
        (long long)seedStart,
        (long long)seedEnd);

    printf("Threads    : %d\n", NUM_THREADS);

    printf("========================================\n");

    for (int i = 0; i < NUM_THREADS; i++)
    {
        uint64_t count =
            base + (i < (int)rem ? 1 : 0);

        args[i].seedStart = current;
        args[i].seedEnd   = current + count - 1;
        args[i].centerX   = centerX;
        args[i].centerZ   = centerZ;
        args[i].radius    = radius;
        args[i].pattern   = pattern;

        current += count;

        printf(
            "Thread %d : %lld -> %lld\n",
            i + 1,
            (long long)args[i].seedStart,
            (long long)args[i].seedEnd);

        if (pthread_create(
                &threads[i],
                NULL,
                worker,
                &args[i]) != 0)
        {
            fprintf(
                stderr,
                "Failed to create thread %d\n",
                i + 1);

            return 1;
        }
    }

    /*
     * Collect worker results.
     */

    for (int i = 0; i < NUM_THREADS; i++)
    {
        void *ret = NULL;

        if (pthread_join(
                threads[i],
                &ret) != 0)
        {
            fprintf(
                stderr,
                "Failed to join thread %d\n",
                i + 1);

            continue;
        }

        results[i] =
            (ThreadResult *)ret;
    }

    /*
     * Combine all worker counters.
     */

    uint64_t seedsScanned = 0;
    uint64_t validWells = 0;
    uint64_t quadwellFound = 0;
    uint64_t threewellSeedsFound = 0;
    uint64_t physical3Found = 0;
    uint64_t physical4Found = 0;

    for (int i = 0; i < NUM_THREADS; i++)
    {
        if (!results[i])
            continue;

        seedsScanned +=
            results[i]->seedsScanned;

        validWells +=
            results[i]->validWells;

        quadwellFound +=
            results[i]->quadwellFound;

        threewellSeedsFound +=
            results[i]->threewellSeedsFound;

        physical3Found +=
            results[i]->physical3Found;

        physical4Found +=
            results[i]->physical4Found;

        /*
         * Keep results[i] alive until the final physical-result
         * output has been printed.
         */
    }

    printf("\n========================================\n");
    printf(" 4-THREAD SEARCH COMPLETE\n");
    printf("========================================\n");

    printf("Seeds scanned    : %llu\n",
        (unsigned long long)seedsScanned);

    printf("Valid wells      : %llu\n",
        (unsigned long long)validWells);

    printf("Quadwells found  : %llu\n",
        (unsigned long long)quadwellFound);

    printf("3-well seeds     : %llu\n",
        (unsigned long long)threewellSeedsFound);

    printf("Physical 3-wells : %llu\n",
        (unsigned long long)physical3Found);

    printf("Physical 4-wells : %llu\n",
        (unsigned long long)physical4Found);

    printf("========================================\n");


    /*
     * ------------------------------------------------------------
     * FINAL PHYSICAL RESULTS
     * ------------------------------------------------------------
     */

    printf("\n========================================\n");
    printf(" PHYSICAL WELL RESULTS\n");
    printf("========================================\n");

    int resultNumber = 0;

    for (int i = 0; i < NUM_THREADS; i++)
    {
        if (!results[i])
            continue;

        for (size_t j = 0;
             j < results[i]->physicalResultCount;
             j++)
        {
            PhysicalResult *pr =
                &results[i]->physicalResults[j];

            Cluster *c = &pr->cluster;

            if (c->count != 3 && c->count != 4)
                continue;

            resultNumber++;

            printf("\n----------------------------------------\n");

            if (c->count == 3)
                printf("PHYSICAL 3-WELL #%d\n", resultNumber);
            else
                printf("PHYSICAL 4-WELL #%d\n", resultNumber);

            printf("Seed : %lld\n",
                   (long long)pr->seed);

            printf("----------------------------------------\n");

            for (int wi = 0; wi < c->count; wi++)
            {
                const Well *w = &c->w[wi];

                printf(
                    "Well %d\n"
                    "  Chunk : (%d, %d)\n"
                    "  World : (%d, %d)\n"
                    "  Local : (%d, %d)\n",
                    wi + 1,
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
        }
    }

    if (resultNumber == 0)
    {
        printf("No physical 3-well or 4-well clusters found.\n");
    }
    else
    {
        printf("\nTotal physical results: %d\n",
               resultNumber);
    }

    /*
     * Free worker result memory after printing.
     */
    for (int i = 0; i < NUM_THREADS; i++)
    {
        if (!results[i])
            continue;

        free(results[i]->physicalResults);
        free(results[i]);
        results[i] = NULL;
    }

    return 0;
}
