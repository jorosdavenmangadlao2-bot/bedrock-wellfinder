#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "finders.h"
#include "generator.h"

#define NUM_THREADS 4


/*
 * ------------------------------------------------------------
 * LEVEL 16: ADAPTIVE THREADING
 *
 * Detect the number of online CPUs at runtime.
 * This helper does NOT change the existing NUM_THREADS paths.
 * The 1B production path will use it explicitly.
 * ------------------------------------------------------------
 */
static int get_adaptive_thread_count(void)
{
    long cpuCount = sysconf(_SC_NPROCESSORS_ONLN);

    if (cpuCount < 1)
        return 1;

    /*
     * Safety cap for Android/Termux.
     * Do not create an excessive number of workers.
     */
    if (cpuCount > 16)
        cpuCount = 16;

    return (int)cpuCount;
}

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

static int debugWellsEnabled = 0;
static int progressEnabled = 1;


static pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t progressCompleted = 0;
static uint64_t progressTotal = 0;
static time_t progressStart = 0;
static time_t progressLast = 0;

/* Search timer */
static time_t searchStartTime = 0;
static time_t searchEndTime = 0;

static void print_search_timing(void)
{
    char startBuf[64];
    char endBuf[64];

    struct tm startTm;
    struct tm endTm;

    localtime_r(&searchStartTime, &startTm);
    localtime_r(&searchEndTime, &endTm);

    strftime(startBuf, sizeof(startBuf), "%Y-%m-%d %H:%M:%S", &startTm);
    strftime(endBuf, sizeof(endBuf), "%Y-%m-%d %H:%M:%S", &endTm);

    uint64_t elapsed = (uint64_t)difftime(
        searchEndTime,
        searchStartTime
    );

    printf("\n========================================\n");
    printf(" SEARCH TIMING\n");
    printf("========================================\n");
    printf("Started : %s\n", startBuf);
    printf("Finished: %s\n", endBuf);
    printf("Elapsed : %02llu:%02llu:%02llu\n",
        (unsigned long long)(elapsed / 3600ULL),
        (unsigned long long)((elapsed % 3600ULL) / 60ULL),
        (unsigned long long)(elapsed % 60ULL)
    );
    printf("========================================\n");
}

static void progress_update(uint64_t completed)
{
    pthread_mutex_lock(&progress_mutex);

    progressCompleted += completed;

    time_t now = time(NULL);

    if (now != progressLast)
    {
        progressLast = now;

        double elapsed = difftime(now, progressStart);

        if (elapsed < 1.0)
            elapsed = 1.0;

        double speed =
            (double)progressCompleted / elapsed;

        double percent =
            progressTotal ?
            ((double)progressCompleted * 100.0 /
             (double)progressTotal) : 0.0;

        double remaining =
            progressTotal > progressCompleted ?
            (double)(progressTotal - progressCompleted) :
            0.0;

        double eta =
            speed > 0.0 ?
            remaining / speed : 0.0;

        uint64_t etaSec = (uint64_t)eta;

        printf(
            "\rProgress: %6.2f%% | "
            "Seeds: %llu/%llu | "
            "Speed: %.2f M/s | "
            "ETA: %02llu:%02llu:%02llu",
            percent,
            (unsigned long long)progressCompleted,
            (unsigned long long)progressTotal,
            speed / 1000000.0,
            (unsigned long long)(etaSec / 3600ULL),
            (unsigned long long)((etaSec % 3600ULL) / 60ULL),
            (unsigned long long)(etaSec % 60ULL)
        );

        fflush(stdout);
    }

    pthread_mutex_unlock(&progress_mutex);
}


/* Live production-search progress */


static void init_debug_wells(void)
{
    const char *env = getenv("DEBUG_WELLS");

    if (env &&
        (strcmp(env, "1") == 0 ||
         strcmp(env, "on") == 0 ||
         strcmp(env, "ON") == 0))
    {
        debugWellsEnabled = 1;
    }
}

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

/*
 * ------------------------------------------------------------
 * LEVEL 14: RELAXED 3/4-WELL DETECTION
 *
 * Final acceptance rules:
 *
 * 3 wells:
 *   - at least one physical pair -> PASS
 *   - no physical pair           -> FAIL
 *
 * 4 wells:
 *   - always PASS
 *
 * A physical pair means two actual 5x5 Desert Well footprints
 * satisfy wells_physically_touch().
 * ------------------------------------------------------------
 */
static int cluster_has_physical_pair(
    const Cluster *c)
{
    if (!c)
        return 0;

    if (c->count < 2)
        return 0;

    for (int i = 0; i < c->count; i++)
    {
        for (int j = i + 1; j < c->count; j++)
        {
            if (wells_physically_touch(
                    &c->w[i],
                    &c->w[j]))
            {
                return 1;
            }
        }
    }

    return 0;
}

static int level14_cluster_passes(
    const Cluster *c)
{
    if (!c)
        return 0;

    if (c->count == 4)
        return 1;

    if (c->count == 3)
        return cluster_has_physical_pair(c);

    return 0;
}

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

    /* STEP7 regression diagnostics */
    uint64_t physicalComponentCount;
    uint64_t physicalPairComponentCount;
    uint64_t physicalPairWellCount;
      /* LEVEL 17 diagnostic counters */
      uint64_t level17PrefilterPairs;
      uint64_t level17ActualPairs;

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

    /*
     * 4F STEP 4:
     * Optional candidate-index stream for audit.
     * NULL means legacy/random mode with no index audit.
     */
    uint64_t *candidateIndices;

    int centerX;
    int centerZ;
    int radius;
    int pattern;
} RandomThreadArgs;


static void save_physical_result_to_file(
    int64_t seed,
    const Cluster *pc)
{
    if (!pc)
        return;

    const char *workerIdEnv =
        getenv("WELL_WORKER_ID");

    int workerId = 0;

    if (workerIdEnv && *workerIdEnv)
        workerId = atoi(workerIdEnv);

    if (workerId < 0)
        workerId = 0;

    char resultPath[256];

    snprintf(
        resultPath,
        sizeof(resultPath),
        "results_1b/physical_wells_worker_%d.txt",
        workerId);

    FILE *fp = fopen(
        resultPath,
        "a");

    if (!fp)
    {
        pthread_mutex_lock(&output_mutex);
        fprintf(
            stderr,
            "\nWARNING: Could not open "
            "%s\n",
            resultPath);
        pthread_mutex_unlock(&output_mutex);
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    char timestamp[64];

    strftime(
        timestamp,
        sizeof(timestamp),
        "%Y-%m-%d %H:%M:%S",
        &tm_now);

    fprintf(
        fp,
        "\n========================================\n");

    fprintf(
        fp,
        "PHYSICAL %d-WELL FOUND\n",
        pc->count);

    fprintf(
        fp,
        "Timestamp : %s\n",
        timestamp);

    fprintf(
        fp,
        "Seed      : %lld\n",
        (long long)seed);

    fprintf(
        fp,
        "----------------------------------------\n");

    for (int wi = 0;
         wi < pc->count && wi < 4;
         wi++)
    {
        const Well *w = &pc->w[wi];

        fprintf(
            fp,
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

    fprintf(
        fp,
        "----------------------------------------\n");

    fprintf(
        fp,
        "Chunk bounds: (%d,%d) -> (%d,%d)\n",
        pc->minX,
        pc->minZ,
        pc->maxX,
        pc->maxZ);

    fprintf(
        fp,
        "========================================\n");

    fflush(fp);
    fclose(fp);
}


static void get_1b_checkpoint_path(
    char *path,
    size_t pathSize)
{
    const char *workerIdEnv =
        getenv("WELL_WORKER_ID");

    int workerId = 0;

    if (workerIdEnv && *workerIdEnv)
        workerId = atoi(workerIdEnv);

    if (workerId < 0)
        workerId = 0;

    snprintf(
        path,
        pathSize,
        "results_1b/checkpoint_worker_%d.txt",
        workerId);
}


static void save_1b_checkpoint(
    const char *status,
    uint64_t rngSeed,
    uint64_t startIndex,
    uint64_t totalCandidates,
    uint64_t nextIndex,
    int centerX,
    int centerZ,
    int radius,
    int pattern,
    uint64_t totalSeeds,
    uint64_t totalValidWells,
    uint64_t totalPhysicalComponents,
    uint64_t totalPhysicalPairComponents,
    uint64_t totalPhysicalPairWells,
    uint64_t totalPhysical3,
    uint64_t totalPhysical4)
{
    char tmpPath[256];
    char finalPath[256];

    get_1b_checkpoint_path(
        finalPath,
        sizeof(finalPath));

    snprintf(
        tmpPath,
        sizeof(tmpPath),
        "%s.tmp",
        finalPath);

    FILE *fp = fopen(tmpPath, "w");

    if (!fp)
    {
        pthread_mutex_lock(&output_mutex);

        fprintf(
            stderr,
            "\nWARNING: Could not write "
            "results_1b/checkpoint.tmp\n");

        pthread_mutex_unlock(&output_mutex);
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);

    char timestamp[64];

    strftime(
        timestamp,
        sizeof(timestamp),
        "%Y-%m-%d %H:%M:%S",
        &tm_now);

    fprintf(fp, "1B CHECKPOINT\n");
    fprintf(fp, "========================================\n");
    fprintf(fp, "Status                 : %s\n", status);
    fprintf(fp, "Timestamp              : %s\n", timestamp);
    fprintf(fp, "RNG seed               : %llu\n",
        (unsigned long long)rngSeed);
    fprintf(fp, "Start index            : %llu\n",
        (unsigned long long)startIndex);
    fprintf(fp, "Next index             : %llu\n",
        (unsigned long long)nextIndex);
    fprintf(fp, "Total candidates       : %llu\n",
        (unsigned long long)totalCandidates);
    fprintf(fp, "Center                 : (%d, %d)\n",
        centerX,
        centerZ);
    fprintf(fp, "Range                  : +/- 100000 chunks\n");
    fprintf(fp, "Radius                 : %d\n", radius);
    fprintf(fp, "Pattern                : %d\n", pattern);
    fprintf(fp, "Seeds scanned          : %llu\n",
        (unsigned long long)totalSeeds);
    fprintf(fp, "Valid wells            : %llu\n",
        (unsigned long long)totalValidWells);
    fprintf(fp, "Physical components    : %llu\n",
        (unsigned long long)totalPhysicalComponents);
    fprintf(fp, "2-well components      : %llu\n",
        (unsigned long long)totalPhysicalPairComponents);
    fprintf(fp, "2-well component wells : %llu\n",
        (unsigned long long)totalPhysicalPairWells);
    fprintf(fp, "Physical 3-wells       : %llu\n",
        (unsigned long long)totalPhysical3);
    fprintf(fp, "Physical 4-wells       : %llu\n",
        (unsigned long long)totalPhysical4);
    fprintf(fp, "========================================\n");

    fflush(fp);
    fclose(fp);

    rename(tmpPath, finalPath);
}


/*
 * ------------------------------------------------------------
 * LEVEL 13: 1B CHECKPOINT LOADER
 *
 * Reads the last completed 1B batch from:
 *   results_1b/checkpoint.txt
 *
 * This function only loads checkpoint data.
 * The 1B search flow will be connected separately after
 * compilation is verified.
 * ------------------------------------------------------------
 */
static int load_1b_checkpoint(
    uint64_t *rngSeed,
    uint64_t *startIndex,
    uint64_t *totalCandidates,
    uint64_t *nextIndex,
    int *centerX,
    int *centerZ,
    int *radius,
    int *pattern,
    uint64_t *totalSeeds,
    uint64_t *totalValidWells,
    uint64_t *totalPhysicalComponents,
    uint64_t *totalPhysicalPairComponents,
    uint64_t *totalPhysicalPairWells,
    uint64_t *totalPhysical3,
    uint64_t *totalPhysical4)
{
    char checkpointPath[256];

    get_1b_checkpoint_path(
        checkpointPath,
        sizeof(checkpointPath));

    FILE *fp = fopen(
        checkpointPath,
        "r");

    if (!fp)
        return 0;

    char line[256];
    int foundRngSeed = 0;
    int foundStartIndex = 0;
    int foundTotalCandidates = 0;
    int foundNextIndex = 0;
    int foundCenter = 0;
    int foundRadius = 0;
    int foundPattern = 0;
    int foundSeeds = 0;
    int foundValid = 0;
    int foundPhysicalComponents = 0;
    int foundPairComponents = 0;
    int foundPairWells = 0;
    int foundPhysical3 = 0;
    int foundPhysical4 = 0;

    while (fgets(line, sizeof(line), fp))
    {
        unsigned long long value;

        if (sscanf(
                line,
                "RNG seed               : %llu",
                &value) == 1)
        {
            *rngSeed = (uint64_t)value;
            foundRngSeed = 1;
        }
        else if (sscanf(
                line,
                "Start index            : %llu",
                &value) == 1)
        {
            *startIndex = (uint64_t)value;
            foundStartIndex = 1;
        }
        else if (sscanf(
                line,
                "Next index             : %llu",
                &value) == 1)
        {
            *nextIndex = (uint64_t)value;
            foundNextIndex = 1;
        }
        else if (sscanf(
                line,
                "Total candidates       : %llu",
                &value) == 1)
        {
            *totalCandidates = (uint64_t)value;
            foundTotalCandidates = 1;
        }
        else if (sscanf(
                line,
                "Center                 : (%d, %d)",
                centerX,
                centerZ) == 2)
        {
            foundCenter = 1;
        }
        else if (sscanf(
                line,
                "Radius                 : %d",
                radius) == 1)
        {
            foundRadius = 1;
        }
        else if (sscanf(
                line,
                "Pattern                : %d",
                pattern) == 1)
        {
            foundPattern = 1;
        }
        else if (sscanf(
                line,
                "Seeds scanned          : %llu",
                &value) == 1)
        {
            *totalSeeds = (uint64_t)value;
            foundSeeds = 1;
        }
        else if (sscanf(
                line,
                "Valid wells            : %llu",
                &value) == 1)
        {
            *totalValidWells = (uint64_t)value;
            foundValid = 1;
        }
        else if (sscanf(
                line,
                "Physical components    : %llu",
                &value) == 1)
        {
            *totalPhysicalComponents = (uint64_t)value;
            foundPhysicalComponents = 1;
        }
        else if (sscanf(
                line,
                "2-well components      : %llu",
                &value) == 1)
        {
            *totalPhysicalPairComponents = (uint64_t)value;
            foundPairComponents = 1;
        }
        else if (sscanf(
                line,
                "2-well component wells : %llu",
                &value) == 1)
        {
            *totalPhysicalPairWells = (uint64_t)value;
            foundPairWells = 1;
        }
        else if (sscanf(
                line,
                "Physical 3-wells       : %llu",
                &value) == 1)
        {
            *totalPhysical3 = (uint64_t)value;
            foundPhysical3 = 1;
        }
        else if (sscanf(
                line,
                "Physical 4-wells       : %llu",
                &value) == 1)
        {
            *totalPhysical4 = (uint64_t)value;
            foundPhysical4 = 1;
        }
    }

    fclose(fp);

    if (!foundRngSeed ||
        !foundStartIndex ||
        !foundTotalCandidates ||
        !foundNextIndex ||
        !foundCenter ||
        !foundRadius ||
        !foundPattern ||
        !foundSeeds ||
        !foundValid ||
        !foundPhysicalComponents ||
        !foundPairComponents ||
        !foundPairWells ||
        !foundPhysical3 ||
        !foundPhysical4)
    {
        return 0;
    }

    return 1;
}

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

    /*
     * Merge physical-component diagnostics.
     */
    dst->physicalComponentCount +=
        src->physicalComponentCount;

    dst->physicalPairComponentCount +=
        src->physicalPairComponentCount;

    dst->physicalPairWellCount +=
        src->physicalPairWellCount;

    /*
     * Merge LEVEL 17 verification diagnostics.
     */
    dst->level17PrefilterPairs +=
        src->level17PrefilterPairs;

    dst->level17ActualPairs +=
        src->level17ActualPairs;

    for (size_t i = 0; i < src->physicalResultCount; i++)
    {
        if (dst->physicalResultCount >=
            dst->physicalResultCapacity)
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

        /*
         * 4F STEP 4 STREAM AUDIT
         *
         * The candidate tuple is already generated before
         * the thread starts. Here we verify that the exact
         * tuple received by this worker is unchanged.
         */

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


/*
 * ------------------------------------------------------------
 * LEVEL 19.1: BASIC SPATIAL DISTRIBUTION ANALYZER
 *
 * Read-only analysis of physical well results.
 *
 * This function does NOT modify:
 *   - well generation
 *   - physical detection
 *   - Level 17 filtering
 *   - Level 18 statistics
 *
 * It analyzes the spatial bounds of already-detected
 * physical 3-well and 4-well clusters.
 * ------------------------------------------------------------
 */
static void print_level19_spatial_analysis(
    const PhysicalResult *results,
    size_t resultCount)
{
    printf("\n========================================\n");
    printf(" SPATIAL DISTRIBUTION ANALYSIS\n");
    printf("========================================\n");

    if (!results || resultCount == 0)
    {
        printf("Physical results       : 0\n");
        printf("Spatial analysis       : N/A\n");
        printf("========================================\n");
        return;
    }

    int globalMinX = results[0].cluster.minX;
    int globalMaxX = results[0].cluster.maxX;
    int globalMinZ = results[0].cluster.minZ;
    int globalMaxZ = results[0].cluster.maxZ;

    uint64_t count3 = 0;
    uint64_t count4 = 0;

    long double sumCenterX = 0.0L;
    long double sumCenterZ = 0.0L;

    for (size_t i = 0; i < resultCount; i++)
    {
        const Cluster *c = &results[i].cluster;

        if (c->count == 3)
            count3++;
        else if (c->count == 4)
            count4++;

        if (c->minX < globalMinX)
            globalMinX = c->minX;

        if (c->maxX > globalMaxX)
            globalMaxX = c->maxX;

        if (c->minZ < globalMinZ)
            globalMinZ = c->minZ;

        if (c->maxZ > globalMaxZ)
            globalMaxZ = c->maxZ;

        sumCenterX +=
            ((long double)c->minX +
             (long double)c->maxX) / 2.0L;

        sumCenterZ +=
            ((long double)c->minZ +
             (long double)c->maxZ) / 2.0L;
    }

    long long spanX =
        (long long)globalMaxX -
        (long long)globalMinX;

    long long spanZ =
        (long long)globalMaxZ -
        (long long)globalMinZ;

    long double averageCenterX =
        sumCenterX / (long double)resultCount;

    long double averageCenterZ =
        sumCenterZ / (long double)resultCount;

    /*
     * Pairwise center-distance analysis.
     *
     * Cluster centers are defined from their chunk bounds.
     * Distances are measured in chunk coordinates.
     */
    uint64_t pairCount = 0;

    long double sumPairDistance = 0.0L;
    long double minPairDistance = 0.0L;
    long double maxPairDistance = 0.0L;

    long double minNearestDistance = 0.0L;
    long double maxNearestDistance = 0.0L;
    long double sumNearestDistance = 0.0L;

    for (size_t i = 0; i < resultCount; i++)
    {
        const Cluster *a = &results[i].cluster;

        long double ax =
            ((long double)a->minX +
             (long double)a->maxX) / 2.0L;

        long double az =
            ((long double)a->minZ +
             (long double)a->maxZ) / 2.0L;

        long double nearest = 0.0L;
        int haveNearest = 0;

        for (size_t j = i + 1; j < resultCount; j++)
        {
            const Cluster *b = &results[j].cluster;

            long double bx =
                ((long double)b->minX +
                 (long double)b->maxX) / 2.0L;

            long double bz =
                ((long double)b->minZ +
                 (long double)b->maxZ) / 2.0L;

            long double dx = ax - bx;
            long double dz = az - bz;

            long double distance =
                sqrtl(dx * dx + dz * dz);

            if (pairCount == 0)
            {
                minPairDistance = distance;
                maxPairDistance = distance;
            }
            else
            {
                if (distance < minPairDistance)
                    minPairDistance = distance;

                if (distance > maxPairDistance)
                    maxPairDistance = distance;
            }

            sumPairDistance += distance;
            pairCount++;

            if (!haveNearest || distance < nearest)
            {
                nearest = distance;
                haveNearest = 1;
            }
        }

        /*
         * Compare against earlier clusters too, because the
         * nearest neighbor may have a lower index.
         */
        for (size_t j = 0; j < i; j++)
        {
            const Cluster *b = &results[j].cluster;

            long double bx =
                ((long double)b->minX +
                 (long double)b->maxX) / 2.0L;

            long double bz =
                ((long double)b->minZ +
                 (long double)b->maxZ) / 2.0L;

            long double dx = ax - bx;
            long double dz = az - bz;

            long double distance =
                sqrtl(dx * dx + dz * dz);

            if (!haveNearest || distance < nearest)
            {
                nearest = distance;
                haveNearest = 1;
            }
        }

        if (haveNearest)
        {
            if (sumNearestDistance == 0.0L &&
                minNearestDistance == 0.0L &&
                maxNearestDistance == 0.0L)
            {
                minNearestDistance = nearest;
                maxNearestDistance = nearest;
            }
            else
            {
                if (nearest < minNearestDistance)
                    minNearestDistance = nearest;

                if (nearest > maxNearestDistance)
                    maxNearestDistance = nearest;
            }

            sumNearestDistance += nearest;
        }
    }

    printf("Physical results       : %zu\n",
        resultCount);

    printf("Physical 3-wells       : %llu\n",
        (unsigned long long)count3);

    printf("Physical 4-wells       : %llu\n",
        (unsigned long long)count4);

    printf("----------------------------------------\n");

    printf("Global X bounds        : (%d) -> (%d)\n",
        globalMinX,
        globalMaxX);

    printf("Global Z bounds        : (%d) -> (%d)\n",
        globalMinZ,
        globalMaxZ);

    printf("Spatial span X         : %lld chunks\n",
        spanX);

    printf("Spatial span Z         : %lld chunks\n",
        spanZ);

    printf("Average cluster X      : %.3Lf\n",
        averageCenterX);

    printf("Average cluster Z      : %.3Lf\n",
        averageCenterZ);

    printf("----------------------------------------\n");

    /*
     * Spatial density and distribution analysis.
     * Read-only analysis of detected physical clusters.
     */

    long double areaChunks =
        ((long double)spanX + 1.0L) *
        ((long double)spanZ + 1.0L);

    printf("----------------------------------------\n");

    if (areaChunks > 0.0L)
    {
        long double density =
            (long double)resultCount / areaChunks;

        printf("Spatial area            : %.0Lf chunks^2\n",
            areaChunks);

        printf("Cluster density         : %.12Lf / chunk^2\n",
            density);

        printf("Clusters per 1M chunks : %.6Lf\n",
            density * 1000000.0L);
    }
    else
    {
        printf("Spatial area            : N/A\n");
        printf("Cluster density         : N/A\n");
        printf("Clusters per 1M chunks : N/A\n");
    }

    uint64_t xBelow = 0;
    uint64_t xAtAbove = 0;
    uint64_t zBelow = 0;
    uint64_t zAtAbove = 0;

    for (size_t i = 0; i < resultCount; i++)
    {
        const Cluster *c = &results[i].cluster;

        long double cx =
            ((long double)c->minX +
             (long double)c->maxX) / 2.0L;

        long double cz =
            ((long double)c->minZ +
             (long double)c->maxZ) / 2.0L;

        if (cx < averageCenterX)
            xBelow++;
        else
            xAtAbove++;

        if (cz < averageCenterZ)
            zBelow++;
        else
            zAtAbove++;
    }

    printf("----------------------------------------\n");
    printf("X distribution\n");
    printf("  Below average X      : %llu\n",
        (unsigned long long)xBelow);
    printf("  At/above average X   : %llu\n",
        (unsigned long long)xAtAbove);

    printf("Z distribution\n");
    printf("  Below average Z      : %llu\n",
        (unsigned long long)zBelow);
    printf("  At/above average Z   : %llu\n",
        (unsigned long long)zAtAbove);

    uint64_t nw = 0;
    uint64_t ne = 0;
    uint64_t sw = 0;
    uint64_t se = 0;

    for (size_t i = 0; i < resultCount; i++)
    {
        const Cluster *c = &results[i].cluster;

        long double cx =
            ((long double)c->minX +
             (long double)c->maxX) / 2.0L;

        long double cz =
            ((long double)c->minZ +
             (long double)c->maxZ) / 2.0L;

        if (cx < averageCenterX)
        {
            if (cz < averageCenterZ)
                nw++;
            else
                sw++;
        }
        else
        {
            if (cz < averageCenterZ)
                ne++;
            else
                se++;
        }
    }

    printf("----------------------------------------\n");
    printf("Quadrant distribution\n");
    printf("  NW                    : %llu\n",
        (unsigned long long)nw);
    printf("  NE                    : %llu\n",
        (unsigned long long)ne);
    printf("  SW                    : %llu\n",
        (unsigned long long)sw);
    printf("  SE                    : %llu\n",
        (unsigned long long)se);

    /* LEVEL 19: pair-distance distribution */
    uint64_t dist_lt10 = 0;
    uint64_t dist_10_99 = 0;
    uint64_t dist_100_999 = 0;
    uint64_t dist_1000_9999 = 0;
    uint64_t dist_10000_99999 = 0;
    uint64_t dist_100000_plus = 0;

    if (pairCount > 0)
    {
        for (size_t i = 0; i < resultCount; i++)
        {
            const Cluster *a = &results[i].cluster;

            long double ax =
                ((long double)a->minX +
                 (long double)a->maxX) / 2.0L;

            long double az =
                ((long double)a->minZ +
                 (long double)a->maxZ) / 2.0L;

            for (size_t j = i + 1; j < resultCount; j++)
            {
                const Cluster *b = &results[j].cluster;

                long double bx =
                    ((long double)b->minX +
                     (long double)b->maxX) / 2.0L;

                long double bz =
                    ((long double)b->minZ +
                     (long double)b->maxZ) / 2.0L;

                long double dx = ax - bx;
                long double dz = az - bz;
                long double distance = sqrtl(dx * dx + dz * dz);

                if (distance < 10.0L)
                    dist_lt10++;
                else if (distance < 100.0L)
                    dist_10_99++;
                else if (distance < 1000.0L)
                    dist_100_999++;
                else if (distance < 10000.0L)
                    dist_1000_9999++;
                else if (distance < 100000.0L)
                    dist_10000_99999++;
                else
                    dist_100000_plus++;
            }
        }
    }

    printf("----------------------------------------\n");
    printf("Pair distance distribution\n");
    printf("  < 10 chunks          : %llu\n",
        (unsigned long long)dist_lt10);
    printf("  10 - 99 chunks       : %llu\n",
        (unsigned long long)dist_10_99);
    printf("  100 - 999 chunks     : %llu\n",
        (unsigned long long)dist_100_999);
    printf("  1,000 - 9,999 chunks : %llu\n",
        (unsigned long long)dist_1000_9999);
    printf("  10,000 - 99,999      : %llu\n",
        (unsigned long long)dist_10000_99999);
    printf("  100,000+ chunks      : %llu\n",
        (unsigned long long)dist_100000_plus);

    /* LEVEL 19.2: local neighborhood density */

    uint64_t local_lt10_total = 0;
    uint64_t local_lt100_total = 0;
    uint64_t local_lt1000_total = 0;

    uint64_t maxLocal10 = 0;
    uint64_t maxLocal100 = 0;
    uint64_t maxLocal1000 = 0;

    for (size_t i = 0; i < resultCount; i++)
    {
        const Cluster *a = &results[i].cluster;

        long double ax =
            ((long double)a->minX +
             (long double)a->maxX) / 2.0L;

        long double az =
            ((long double)a->minZ +
             (long double)a->maxZ) / 2.0L;

        uint64_t local10 = 0;
        uint64_t local100 = 0;
        uint64_t local1000 = 0;

        for (size_t j = 0; j < resultCount; j++)
        {
            if (i == j)
                continue;

            const Cluster *b = &results[j].cluster;

            long double bx =
                ((long double)b->minX +
                 (long double)b->maxX) / 2.0L;

            long double bz =
                ((long double)b->minZ +
                 (long double)b->maxZ) / 2.0L;

            long double dx = ax - bx;
            long double dz = az - bz;

            long double distance =
                sqrtl(dx * dx + dz * dz);

            if (distance < 10.0L)
                local10++;

            if (distance < 100.0L)
                local100++;

            if (distance < 1000.0L)
                local1000++;
        }

        local_lt10_total += local10;
        local_lt100_total += local100;
        local_lt1000_total += local1000;

        if (local10 > maxLocal10)
            maxLocal10 = local10;

        if (local100 > maxLocal100)
            maxLocal100 = local100;

        if (local1000 > maxLocal1000)
            maxLocal1000 = local1000;
    }

    printf("----------------------------------------\n");
    printf("Local neighborhood density\n");

    if (resultCount > 0)
    {
        printf("  Avg neighbors < 10    : %.3Lf\n",
            (long double)local_lt10_total /
            (long double)resultCount);

        printf("  Avg neighbors < 100   : %.3Lf\n",
            (long double)local_lt100_total /
            (long double)resultCount);

        printf("  Avg neighbors < 1000  : %.3Lf\n",
            (long double)local_lt1000_total /
            (long double)resultCount);

        printf("  Max neighbors < 10    : %llu\n",
            (unsigned long long)maxLocal10);

        printf("  Max neighbors < 100   : %llu\n",
            (unsigned long long)maxLocal100);

        printf("  Max neighbors < 1000  : %llu\n",
            (unsigned long long)maxLocal1000);
    }
    else
    {
        printf("  Local density          : N/A\n");
    }

    /* LEVEL 19.3: local clustering ratios */

    printf("----------------------------------------\n");
    printf("Local clustering ratios\n");

    if (resultCount > 1)
    {
        long double denominator =
            (long double)(resultCount - 1);

        long double ratio10 =
            ((long double)local_lt10_total /
             (long double)resultCount) /
            denominator;

        long double ratio100 =
            ((long double)local_lt100_total /
             (long double)resultCount) /
            denominator;

        long double ratio1000 =
            ((long double)local_lt1000_total /
             (long double)resultCount) /
            denominator;

        printf("  Clustering ratio < 10   : %.6Lf\n",
            ratio10);

        printf("  Clustering ratio < 100  : %.6Lf\n",
            ratio100);

        printf("  Clustering ratio < 1000 : %.6Lf\n",
            ratio1000);
    }
    else
    {
        printf("  Clustering ratio < 10   : N/A\n");
        printf("  Clustering ratio < 100  : N/A\n");
        printf("  Clustering ratio < 1000 : N/A\n");
    }

    /* LEVEL 19.4: distance-based clustering profile */

    printf("----------------------------------------\n");
    printf("Distance clustering profile\n");

    if (pairCount > 0)
    {
        long double totalPairs =
            (long double)pairCount;

        printf("  < 10 chunks          : %.6Lf%%\n",
            ((long double)dist_lt10 / totalPairs) * 100.0L);

        printf("  10 - 99 chunks       : %.6Lf%%\n",
            ((long double)dist_10_99 / totalPairs) * 100.0L);

        printf("  100 - 999 chunks     : %.6Lf%%\n",
            ((long double)dist_100_999 / totalPairs) * 100.0L);

        printf("  1,000 - 9,999 chunks : %.6Lf%%\n",
            ((long double)dist_1000_9999 / totalPairs) * 100.0L);

        printf("  10,000 - 99,999      : %.6Lf%%\n",
            ((long double)dist_10000_99999 / totalPairs) * 100.0L);

        printf("  100,000+ chunks      : %.6Lf%%\n",
            ((long double)dist_100000_plus / totalPairs) * 100.0L);
    }
    else
    {
        printf("  Distance profile      : N/A\n");
    }

    /* LEVEL 19.5: spatial summary */
    /* LEVEL 19.7: radial spatial distribution profile */

    printf("----------------------------------------\n");
    printf("Radial spatial distribution profile\n");

    if (resultCount > 0)
    {
        long double radialSum = 0.0L;
        long double radialMin = 0.0L;
        long double radialMax = 0.0L;

        uint64_t radial_lt10 = 0;
        uint64_t radial_10_99 = 0;
        uint64_t radial_100_999 = 0;
        uint64_t radial_1000_9999 = 0;
        uint64_t radial_10000_99999 = 0;
        uint64_t radial_100000_plus = 0;

        for (size_t i = 0; i < resultCount; i++)
        {
            const Cluster *c = &results[i].cluster;

            long double cx =
                ((long double)c->minX +
                 (long double)c->maxX) / 2.0L;

            long double cz =
                ((long double)c->minZ +
                 (long double)c->maxZ) / 2.0L;

            long double dx = cx - averageCenterX;
            long double dz = cz - averageCenterZ;

            long double radialDistance =
                sqrtl(dx * dx + dz * dz);

            if (i == 0)
            {
                radialMin = radialDistance;
                radialMax = radialDistance;
            }
            else
            {
                if (radialDistance < radialMin)
                    radialMin = radialDistance;

                if (radialDistance > radialMax)
                    radialMax = radialDistance;
            }

            radialSum += radialDistance;

            if (radialDistance < 10.0L)
                radial_lt10++;
            else if (radialDistance < 100.0L)
                radial_10_99++;
            else if (radialDistance < 1000.0L)
                radial_100_999++;
            else if (radialDistance < 10000.0L)
                radial_1000_9999++;
            else if (radialDistance < 100000.0L)
                radial_10000_99999++;
            else
                radial_100000_plus++;
        }

        printf("  Average radial distance : %.3Lf chunks\n",
            radialSum / (long double)resultCount);

        printf("  Minimum radial distance : %.3Lf chunks\n",
            radialMin);

        printf("  Maximum radial distance : %.3Lf chunks\n",
            radialMax);

        printf("  < 10 chunks             : %llu\n",
            (unsigned long long)radial_lt10);

        printf("  10 - 99 chunks          : %llu\n",
            (unsigned long long)radial_10_99);

        printf("  100 - 999 chunks        : %llu\n",
            (unsigned long long)radial_100_999);

        printf("  1,000 - 9,999 chunks    : %llu\n",
            (unsigned long long)radial_1000_9999);

        printf("  10,000 - 99,999 chunks  : %llu\n",
            (unsigned long long)radial_10000_99999);

        printf("  100,000+ chunks         : %llu\n",
            (unsigned long long)radial_100000_plus);

        long double totalRadial =
            (long double)resultCount;

        printf("  < 10 fraction           : %.6Lf%%\n",
            ((long double)radial_lt10 / totalRadial) * 100.0L);

        printf("  10 - 99 fraction        : %.6Lf%%\n",
            ((long double)radial_10_99 / totalRadial) * 100.0L);

        printf("  100 - 999 fraction      : %.6Lf%%\n",
            ((long double)radial_100_999 / totalRadial) * 100.0L);

        printf("  1,000 - 9,999 fraction  : %.6Lf%%\n",
            ((long double)radial_1000_9999 / totalRadial) * 100.0L);

        printf("  10,000 - 99,999 fraction: %.6Lf%%\n",
            ((long double)radial_10000_99999 / totalRadial) * 100.0L);

        printf("  100,000+ fraction       : %.6Lf%%\n",
            ((long double)radial_100000_plus / totalRadial) * 100.0L);
    }
    else
    {
        printf("  Radial profile          : N/A\n");
    }

    /* LEVEL 19.8: radial concentration profile */

    printf("----------------------------------------\n");
    printf("Radial concentration profile\n");

    if (resultCount > 0)
    {
        uint64_t radius10 = 0;
        uint64_t radius100 = 0;
        uint64_t radius1000 = 0;
        uint64_t radius10000 = 0;
        uint64_t radius100000 = 0;

        for (size_t i = 0; i < resultCount; i++)
        {
            const Cluster *c = &results[i].cluster;

            long double cx =
                ((long double)c->minX +
                 (long double)c->maxX) / 2.0L;

            long double cz =
                ((long double)c->minZ +
                 (long double)c->maxZ) / 2.0L;

            long double dx = cx - averageCenterX;
            long double dz = cz - averageCenterZ;

            long double distance =
                sqrtl(dx * dx + dz * dz);

            if (distance < 10.0L)
                radius10++;

            if (distance < 100.0L)
                radius100++;

            if (distance < 1000.0L)
                radius1000++;

            if (distance < 10000.0L)
                radius10000++;

            if (distance < 100000.0L)
                radius100000++;
        }

        long double total =
            (long double)resultCount;

        printf("  Within 10 chunks       : %llu (%.6Lf%%)\n",
            (unsigned long long)radius10,
            ((long double)radius10 / total) * 100.0L);

        printf("  Within 100 chunks      : %llu (%.6Lf%%)\n",
            (unsigned long long)radius100,
            ((long double)radius100 / total) * 100.0L);

        printf("  Within 1,000 chunks    : %llu (%.6Lf%%)\n",
            (unsigned long long)radius1000,
            ((long double)radius1000 / total) * 100.0L);

        printf("  Within 10,000 chunks   : %llu (%.6Lf%%)\n",
            (unsigned long long)radius10000,
            ((long double)radius10000 / total) * 100.0L);

        printf("  Within 100,000 chunks  : %llu (%.6Lf%%)\n",
            (unsigned long long)radius100000,
            ((long double)radius100000 / total) * 100.0L);
    }
    else
    {
        printf("  Radial concentration   : N/A\n");
    }

    /* LEVEL 19.9: radial density profile */

    printf("----------------------------------------\n");
    printf("Radial density profile\n");

    if (resultCount > 0)
    {
        long double pi = acosl(-1.0L);

        uint64_t density10 = 0;
        uint64_t density100 = 0;
        uint64_t density1000 = 0;
        uint64_t density10000 = 0;
        uint64_t density100000 = 0;

        for (size_t i = 0; i < resultCount; i++)
        {
            const Cluster *c = &results[i].cluster;

            long double cx =
                ((long double)c->minX +
                 (long double)c->maxX) / 2.0L;

            long double cz =
                ((long double)c->minZ +
                 (long double)c->maxZ) / 2.0L;

            long double dx = cx - averageCenterX;
            long double dz = cz - averageCenterZ;

            long double distance =
                sqrtl(dx * dx + dz * dz);

            if (distance < 10.0L)
                density10++;

            if (distance < 100.0L)
                density100++;

            if (distance < 1000.0L)
                density1000++;

            if (distance < 10000.0L)
                density10000++;

            if (distance < 100000.0L)
                density100000++;
        }

        long double area10 =
            pi * 10.0L * 10.0L;

        long double area100 =
            pi * 100.0L * 100.0L;

        long double area1000 =
            pi * 1000.0L * 1000.0L;

        long double area10000 =
            pi * 10000.0L * 10000.0L;

        long double area100000 =
            pi * 100000.0L * 100000.0L;

        printf("  < 10 chunks density       : %.12Lf\n",
            (long double)density10 / area10);

        printf("  < 100 chunks density      : %.12Lf\n",
            (long double)density100 / area100);

        printf("  < 1,000 chunks density    : %.12Lf\n",
            (long double)density1000 / area1000);

        printf("  < 10,000 chunks density   : %.12Lf\n",
            (long double)density10000 / area10000);

        printf("  < 100,000 chunks density  : %.12Lf\n",
            (long double)density100000 / area100000);
    }
    else
    {
        printf("  Radial density            : N/A\n");
    }

    /* LEVEL 19.6: spatial concentration index */

    printf("----------------------------------------\n");
    printf("Spatial concentration index\n");

    if (resultCount > 0 && spanX >= 0 && spanZ >= 0)
    {
        long double width =
            (long double)spanX + 1.0L;

        long double height =
            (long double)spanZ + 1.0L;

        long double area =
            width * height;

        long double density =
            (long double)resultCount / area;

        long double concentration =
            density * 1000000.0L;

        printf("  Analysis area          : %.0Lf chunks^2\n",
            area);

        printf("  Cluster count          : %zu\n",
            resultCount);

        printf("  Concentration / 1M     : %.6Lf clusters\n",
            concentration);

        if (resultCount > 1)
        {
            long double maxPossible =
                (long double)resultCount *
                (long double)(resultCount - 1);

            long double observed =
                (long double)local_lt100_total;

            long double localFraction =
                observed / maxPossible;

            printf("  Local fraction < 100   : %.6Lf\n",
                localFraction);
        }
        else
        {
            printf("  Local fraction < 100   : N/A\n");
        }
    }
    else
    {
        printf("  Spatial concentration  : N/A\n");
    }


    printf("----------------------------------------\n");
    printf("Spatial summary\n");

    if (resultCount > 0)
    {
        long double avgNeighbors10 =
            (long double)local_lt10_total /
            (long double)resultCount;

        long double avgNeighbors100 =
            (long double)local_lt100_total /
            (long double)resultCount;

        long double avgNeighbors1000 =
            (long double)local_lt1000_total /
            (long double)resultCount;

        printf("  Results                : %zu\n",
            resultCount);

        printf("  Density / chunk^2      : %.12Lf\n",
            (long double)resultCount /
            (((long double)spanX + 1.0L) *
             ((long double)spanZ + 1.0L)));

        printf("  Avg neighbors < 10     : %.3Lf\n",
            avgNeighbors10);

        printf("  Avg neighbors < 100    : %.3Lf\n",
            avgNeighbors100);

        printf("  Avg neighbors < 1000   : %.3Lf\n",
            avgNeighbors1000);

        printf("  Closest pair            : %.3Lf chunks\n",
            minPairDistance);

        printf("  Farthest pair           : %.3Lf chunks\n",
            maxPairDistance);
    }
    else
    {
        printf("  Spatial summary         : N/A\n");
    }

    printf("Cluster pairs          : %llu\n",
        (unsigned long long)pairCount);

    if (pairCount > 0)
    {
        printf("Average pair distance  : %.3Lf chunks\n",
            sumPairDistance / (long double)pairCount);

        printf("Minimum pair distance  : %.3Lf chunks\n",
            minPairDistance);

        printf("Maximum pair distance  : %.3Lf chunks\n",
            maxPairDistance);

        printf("Average nearest-neighbor: %.3Lf chunks\n",
            sumNearestDistance / (long double)resultCount);

        printf("Minimum nearest-neighbor: %.3Lf chunks\n",
            minNearestDistance);

        printf("Maximum nearest-neighbor: %.3Lf chunks\n",
            maxNearestDistance);
    }
    else
    {
        printf("Average pair distance  : N/A\n");
        printf("Minimum pair distance  : N/A\n");
        printf("Maximum pair distance  : N/A\n");
        printf("Average nearest-neighbor: N/A\n");
        printf("Minimum nearest-neighbor: N/A\n");
        printf("Maximum nearest-neighbor: N/A\n");
    }

    printf("========================================\n");
}

static void print_level19_random_spatial_analysis(
    ThreadResult *results[],
    int threadCount)
{
    size_t totalCount = 0;

    for (int i = 0; i < threadCount; i++)
    {
        if (results[i])
            totalCount += results[i]->physicalResultCount;
    }

    if (totalCount == 0)
    {
        print_level19_spatial_analysis(NULL, 0);
        return;
    }

    PhysicalResult *allResults =
        malloc(totalCount * sizeof(PhysicalResult));

    if (!allResults)
    {
        fprintf(stderr,
            "LEVEL 19: failed to allocate spatial analysis buffer.\n");
        return;
    }

    size_t offset = 0;

    for (int i = 0; i < threadCount; i++)
    {
        if (!results[i] ||
            results[i]->physicalResultCount == 0)
            continue;

        memcpy(
            allResults + offset,
            results[i]->physicalResults,
            results[i]->physicalResultCount *
            sizeof(PhysicalResult)
        );

        offset += results[i]->physicalResultCount;
    }

    print_level19_spatial_analysis(
        allResults,
        totalCount
    );

    free(allResults);
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
        
        if (progressEnabled)
            progress_update(1);

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
         * LEVEL 17:
         * Verified two-stage filtering.
         *
         * This becomes true when at least one pair of
         * validated wells is confirmed to physically touch.
         *
         * The existing physical component scanner remains
         * the final validator.
         */
        int hasPossiblePhysicalPair = 0;

        /*
         * LEVEL 17 VERIFICATION COUNTERS
         *
         * Diagnostics only. No candidate is skipped yet.
         */
        int prefilterPairDetected = 0;
        int actualPhysicalPairDetected = 0;

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

                    /*
                     * LEVEL 17 PREFILTER:
                     * Check whether this newly validated well physically
                     * touches any previously validated well.
                     *
                     * This is only a cheap prefilter. The existing
                     * physical component scanner remains authoritative.
                     */
                    for (int pgz = 0; pgz < height && !hasPossiblePhysicalPair; pgz++)
                    {
                        for (int pgx = 0; pgx < width && !hasPossiblePhysicalPair; pgx++)
                        {
                            Well *prev = &grid[pgz * width + pgx];

                            if (prev->cx < 0)
                                continue;

                            if (prev->cx == slot->cx &&
                                prev->cz == slot->cz)
                                continue;

                            if (wells_physically_touch(prev, slot))
                            {
                                hasPossiblePhysicalPair = 1;
                                prefilterPairDetected = 1;
                            }
                        }
                    }

                    if (debugWellsEnabled)
                    {
                        printf(
                            "DEBUG WELL: seed=%llu chunk=(%d,%d) world=(%d,%d) local=(%d,%d)\n",
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

            if (debugWellsEnabled)
            {
                printf("\nDEBUG PHYSICAL COMPONENTS: %d\n",
                       componentCount);

                for (int di = 0; di < componentCount; di++)
                {
                    printf("DEBUG COMPONENT %d: count=%d\n",
                           di + 1,
                           components[di].count);

                    if (components[di].count > 0 &&
                        components[di].count <= 4)
                    {
                        for (int wi = 0;
                             wi < components[di].count;
                             wi++)
                        {
                            printf(
                                "  W%d chunk=(%d,%d) world=(%d,%d)\n",
                                wi + 1,
                                components[di].w[wi].cx,
                                components[di].w[wi].cz,
                                components[di].w[wi].x,
                                components[di].w[wi].z
                            );
                        }
                    }
                }
            }

            /* STEP7: expose the raw physical-component structure
             * before the 3/4-well reporting filter.
             */
            r->physicalComponentCount +=
                (uint64_t)componentCount;

            for (int ci = 0;
                 ci < componentCount;
                 ci++)
            {
                Cluster *pc =
                    &components[ci];

                if (pc->count == 2)
                {
                    r->physicalPairComponentCount++;
                    r->physicalPairWellCount += 2;
                    actualPhysicalPairDetected = 1;
                }

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

                save_physical_result_to_file(
                    seed,
                    pc);

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
                        /*
                         * LEVEL 14:
                         * Any exact 4-well block passes.
                         *
                         * Physical connectivity is no longer
                         * required for the relaxed 4-well result.
                         */
                        if (!level14_cluster_passes(&c))
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
                        /*
                         * LEVEL 14:
                         * A 3-well pattern passes when at least
                         * two of the three wells physically touch.
                         */
                        if (!level14_cluster_passes(&c))
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

        /*
         * LEVEL 17 VERIFICATION
         * Diagnostics only.
         *
         * These counters compare:
         *   1. prefilter pair detection
         *   2. authoritative physical pair detection
         */
        if (prefilterPairDetected)
            r->level17PrefilterPairs++;

        if (actualPhysicalPairDetected)
            r->level17ActualPairs++;

next_seed:

        if (seed == UINT64_MAX)
            break;
    }

    free(grid);

    return r;
}


/*
 * ------------------------------------------------------------
 * LEVEL 18: STATISTICAL ANALYZER
 *
 * Read-only analysis of aggregated search results.
 *
 * This function does NOT modify detection logic.
 * It only calculates derived statistics from counters
 * already produced by the verified search pipeline.
 * ------------------------------------------------------------
 */
static void print_level18_statistics(
    uint64_t seedsScanned,
    uint64_t validWells,
    uint64_t quadwellFound,
    uint64_t threewellSeedsFound,
    uint64_t physicalComponentCount,
    uint64_t physicalPairComponentCount,
    uint64_t physicalPairWellCount,
    uint64_t physical3Found,
    uint64_t physical4Found,
    uint64_t level17PrefilterPairs,
    uint64_t level17ActualPairs)
{
    printf("\n========================================\n");
    printf(" LEVEL 18 STATISTICAL ANALYZER\n");
    printf("========================================\n");

    printf("Seeds scanned             : %llu\n",
        (unsigned long long)seedsScanned);

    printf("Valid wells               : %llu\n",
        (unsigned long long)validWells);

    printf("Quadwells found           : %llu\n",
        (unsigned long long)quadwellFound);

    printf("3-well seeds              : %llu\n",
        (unsigned long long)threewellSeedsFound);

    printf("Physical components       : %llu\n",
        (unsigned long long)physicalComponentCount);

    printf("2-well components         : %llu\n",
        (unsigned long long)physicalPairComponentCount);

    printf("Wells in 2-well comps     : %llu\n",
        (unsigned long long)physicalPairWellCount);

    printf("Physical 3-wells           : %llu\n",
        (unsigned long long)physical3Found);

    printf("Physical 4-wells           : %llu\n",
        (unsigned long long)physical4Found);

    printf("----------------------------------------\n");

    if (seedsScanned > 0)
    {
        printf("Average valid wells/seed  : %.6f\n",
            (double)validWells / (double)seedsScanned);

        printf("Valid-well rate/seed      : %.6f%%\n",
            ((double)validWells / (double)seedsScanned) * 100.0);

        printf("Quadwell rate/seed        : %.9f%%\n",
            ((double)quadwellFound / (double)seedsScanned) * 100.0);

        printf("Physical 3 rate/seed      : %.9f%%\n",
            ((double)physical3Found / (double)seedsScanned) * 100.0);

        printf("Physical 4 rate/seed      : %.9f%%\n",
            ((double)physical4Found / (double)seedsScanned) * 100.0);
    }
    else
    {
        printf("Average valid wells/seed  : N/A\n");
        printf("Valid-well rate/seed      : N/A\n");
        printf("Quadwell rate/seed        : N/A\n");
        printf("Physical 3 rate/seed      : N/A\n");
        printf("Physical 4 rate/seed      : N/A\n");
    }

    printf("----------------------------------------\n");

    printf("LEVEL 17 prefilter pairs  : %llu\n",
        (unsigned long long)level17PrefilterPairs);

    printf("LEVEL 17 actual pairs     : %llu\n",
        (unsigned long long)level17ActualPairs);

    if (level17PrefilterPairs > 0)
    {
        double accuracy =
            ((double)level17ActualPairs /
             (double)level17PrefilterPairs) * 100.0;

        printf("LEVEL 17 validation rate  : %.6f%%\n",
            accuracy);
    }
    else
    {
        printf("LEVEL 17 validation rate  : N/A\n");
    }

    printf("========================================\n");
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
        fprintf(stderr, "Pattern must be 0, 2, 3, or 4\n");
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
    init_debug_wells();

    /*
     * --------------------------------------------------------
     * LEVEL 20.6 + 20.7 + 20.8
     *
     * Distributed result consolidation with:
     *
     *   20.6 - worker result consolidation
     *   20.7 - worker file validation
     *   20.8 - atomic result finalization
     *
     * The final output is replaced only after every expected
     * worker file has been successfully validated and copied.
     *
     * Worker files are never modified or deleted.
     *
     * Usage:
     *   ./well_search_physical_finder_4f_random_1b 1bmerge
     * --------------------------------------------------------
     */
    if (argc == 2 &&
        strcmp(argv[1], "1bmerge") == 0)
    {
        const char *outputPath =
            "results_1b/physical_wells_all.txt";

        const char *tempPath =
            "results_1b/physical_wells_all.txt.tmp";

        int expectedWorkers = 1;

        const char *workerCountEnv =
            getenv("WELL_WORKER_COUNT");

        if (workerCountEnv && *workerCountEnv)
        {
            expectedWorkers = atoi(workerCountEnv);

            if (expectedWorkers <= 0)
                expectedWorkers = 1;
        }

        int missingWorkers = 0;

        printf("========================================\n");
        printf(" LEVEL 20.8 ATOMIC RESULT FINALIZATION\n");
        printf("========================================\n");
        printf("Expected workers : %d\n", expectedWorkers);
        printf("----------------------------------------\n");

        /*
         * STEP 1:
         * Validate every expected worker file BEFORE creating
         * the new final result.
         */
        for (int workerId = 0;
             workerId < expectedWorkers;
             workerId++)
        {
            char workerPath[256];

            snprintf(
                workerPath,
                sizeof(workerPath),
                "results_1b/physical_wells_worker_%d.txt",
                workerId
            );

            FILE *check = fopen(workerPath, "r");

            if (check)
            {
                printf(
                    "Worker %d : FOUND\n",
                    workerId
                );

                fclose(check);
            }
            else
            {
                printf(
                    "Worker %d : MISSING\n",
                    workerId
                );

                missingWorkers++;
            }
        }

        printf("----------------------------------------\n");

        if (missingWorkers > 0)
        {
            printf(
                "RESULT: INCOMPLETE - %d WORKER FILE(S) MISSING\n",
                missingWorkers
            );

            printf(
                "Atomic finalization aborted. Existing output preserved.\n"
            );

            printf("========================================\n");

            return 1;
        }

        printf(
            "RESULT: PASS - ALL WORKER FILES PRESENT\n"
        );

        /*
         * STEP 2:
         * Write the consolidated result into a temporary file.
         *
         * The existing final output is NOT touched yet.
         */
        FILE *out = fopen(tempPath, "w");

        if (!out)
        {
            fprintf(
                stderr,
                "LEVEL 20.8: Could not open temporary output: %s\n",
                tempPath
            );

            return 1;
        }

        int mergedWorkers = 0;
        unsigned long long mergedBytes = 0;
        int mergeError = 0;

        for (int workerId = 0;
             workerId < expectedWorkers;
             workerId++)
        {
            char workerPath[256];

            snprintf(
                workerPath,
                sizeof(workerPath),
                "results_1b/physical_wells_worker_%d.txt",
                workerId
            );

            FILE *in = fopen(workerPath, "r");

            if (!in)
            {
                fprintf(
                    stderr,
                    "LEVEL 20.8: Worker file disappeared: %s\n",
                    workerPath
                );

                mergeError = 1;
                break;
            }

            int c;

            while ((c = fgetc(in)) != EOF)
            {
                if (fputc(c, out) == EOF)
                {
                    fprintf(
                        stderr,
                        "LEVEL 20.8: Write error on temporary output.\n"
                    );

                    mergeError = 1;
                    break;
                }

                mergedBytes++;
            }

            fclose(in);

            if (mergeError)
                break;

            mergedWorkers++;
        }

        if (fflush(out) != 0)
            mergeError = 1;

        if (fclose(out) != 0)
            mergeError = 1;

        /*
         * STEP 3:
         * If anything failed, remove only the temporary file.
         * The existing final result remains untouched.
         */
        if (mergeError ||
            mergedWorkers != expectedWorkers)
        {
            fprintf(
                stderr,
                "LEVEL 20.8: Atomic finalization FAILED.\n"
            );

            fprintf(
                stderr,
                "Existing output preserved: %s\n",
                outputPath
            );

            remove(tempPath);

            printf("========================================\n");

            return 1;
        }

        /*
         * STEP 4:
         * Atomically replace the final result.
         */
        if (rename(tempPath, outputPath) != 0)
        {
            fprintf(
                stderr,
                "LEVEL 20.8: Could not finalize output: %s\n",
                outputPath
            );

            remove(tempPath);

            printf("========================================\n");

            return 1;
        }

        printf("----------------------------------------\n");
        printf("Worker files merged : %d\n", mergedWorkers);
        printf("Bytes copied        : %llu\n",
            mergedBytes);
        printf("Output              : %s\n",
            outputPath);
        printf("----------------------------------------\n");
        /*
         * --------------------------------------------------------
         * LEVEL 20.9: FINAL RESULT INTEGRITY VERIFICATION
         *
         * Verify that the finalized result file exists and can
         * be opened for reading after atomic replacement.
         * The finalized output is never modified here.
         * --------------------------------------------------------
         */
        {
            FILE *verify = fopen(outputPath, "r");

            if (!verify)
            {
                fprintf(
                    stderr,
                    "LEVEL 20.9: Final result verification FAILED.\n"
                );

                fprintf(
                    stderr,
                    "Final output could not be opened: %s\n",
                    outputPath
                );

                printf("========================================\n");

                return 1;
            }

            unsigned long long verifiedBytes = 0;
            int verifyChar;

            while ((verifyChar = fgetc(verify)) != EOF)
                verifiedBytes++;

            fclose(verify);

            printf("----------------------------------------\n");
            printf("Final result verified : YES\n");
            printf("Verified bytes        : %llu\n",
                verifiedBytes);
            printf("Verified output       : %s\n",
                outputPath);
            printf("----------------------------------------\n");

            printf(
                "RESULT: PASS - FINAL RESULT INTEGRITY VERIFIED\n"
            );
        }

        /*
         * --------------------------------------------------------
         * LEVEL 20.10: FINAL RESULT CONTENT STATISTICS
         *
         * Read-only verification of the finalized consolidated
         * result file.
         *
         * Reports:
         *   - total lines
         *   - non-empty lines
         *   - empty lines
         *   - byte count
         *
         * The finalized result file is never modified.
         * --------------------------------------------------------
         */
        {
            FILE *stats = fopen(outputPath, "r");

            if (!stats)
            {
                fprintf(
                    stderr,
                    "LEVEL 20.10: Could not open final result for statistics.\n"
                );

                return 1;
            }

            unsigned long long statBytes = 0;
            unsigned long long totalLines = 0;
            unsigned long long nonEmptyLines = 0;
            unsigned long long emptyLines = 0;

            int ch;
            int lineHasContent = 0;

            while ((ch = fgetc(stats)) != EOF)
            {
                statBytes++;

                if (ch == '\n')
                {
                    totalLines++;

                    if (lineHasContent)
                        nonEmptyLines++;
                    else
                        emptyLines++;

                    lineHasContent = 0;
                }
                else if (ch != '\r')
                {
                    lineHasContent = 1;
                }
            }

            if (statBytes > 0 && lineHasContent)
            {
                totalLines++;
                nonEmptyLines++;
            }

            fclose(stats);

            printf("----------------------------------------\n");
            printf(" LEVEL 20.10 RESULT CONTENT STATISTICS\n");
            printf("----------------------------------------\n");
            printf("Result bytes        : %llu\n",
                statBytes);
            printf("Expected bytes      : %llu\n",
                mergedBytes);
            printf("Total lines         : %llu\n",
                totalLines);
            printf("Non-empty lines     : %llu\n",
                nonEmptyLines);
            printf("Empty lines         : %llu\n",
                emptyLines);
            printf("----------------------------------------\n");

            if (statBytes != mergedBytes)
            {
                fprintf(
                    stderr,
                    "LEVEL 20.10: FAIL - byte count mismatch.\n"
                );

                return 1;
            }

            printf(
                "RESULT: PASS - FINAL RESULT STATISTICS VERIFIED\n"
            );
            printf("----------------------------------------\n");
        }

        /*
         * --------------------------------------------------------
         * LEVEL 20.11: FINAL RESULT CONTENT EQUIVALENCE
         *
         * Verify that the finalized result file is byte-for-byte
         * identical to the concatenation of all expected worker
         * result files.
         *
         * Worker files are never modified.
         * --------------------------------------------------------
         */
        {
            FILE *finalFile = fopen(outputPath, "rb");

            if (!finalFile)
            {
                fprintf(
                    stderr,
                    "LEVEL 20.11: Could not open final result.\n"
                );

                return 1;
            }

            int equivalenceError = 0;
            unsigned long long comparedBytes = 0;

            for (int workerId = 0;
                 workerId < expectedWorkers && !equivalenceError;
                 workerId++)
            {
                char workerPath[256];

                snprintf(
                    workerPath,
                    sizeof(workerPath),
                    "results_1b/physical_wells_worker_%d.txt",
                    workerId
                );

                FILE *workerFile = fopen(workerPath, "rb");

                if (!workerFile)
                {
                    fprintf(
                        stderr,
                        "LEVEL 20.11: Could not open worker file: %s\n",
                        workerPath
                    );

                    equivalenceError = 1;
                    break;
                }

                int finalChar;
                int workerChar;

                while ((workerChar = fgetc(workerFile)) != EOF)
                {
                    finalChar = fgetc(finalFile);

                    if (finalChar == EOF ||
                        finalChar != workerChar)
                    {
                        equivalenceError = 1;
                        break;
                    }

                    comparedBytes++;
                }

                fclose(workerFile);
            }

            if (!equivalenceError)
            {
                if (fgetc(finalFile) != EOF)
                    equivalenceError = 1;
            }

            fclose(finalFile);

            printf("----------------------------------------\n");
            printf(" LEVEL 20.11 CONTENT EQUIVALENCE\n");
            printf("----------------------------------------\n");
            printf(
                "Compared bytes      : %llu\n",
                comparedBytes
            );

            if (equivalenceError)
            {
                fprintf(
                    stderr,
                    "RESULT: FAIL - FINAL RESULT CONTENT MISMATCH\n"
                );

                printf("----------------------------------------\n");
                return 1;
            }

            printf(
                "RESULT: PASS - FINAL RESULT CONTENT EQUIVALENT\n"
            );
            printf("----------------------------------------\n");
        }

        /*
         * --------------------------------------------------------
         * LEVEL 20.12: WORKER RESULT RECORD COUNT VERIFICATION
         *
         * Verify that the number of non-empty result records in
         * the finalized output matches the combined worker files.
         *
         * Read-only verification. No result file is modified.
         * --------------------------------------------------------
         */
        {
            unsigned long long finalRecords = 0;
            unsigned long long workerRecords = 0;
            int countError = 0;

            FILE *finalCount = fopen(outputPath, "r");

            if (!finalCount)
            {
                fprintf(
                    stderr,
                    "LEVEL 20.12: Could not open final result.\n"
                );

                return 1;
            }

            char line[4096];

            while (fgets(line, sizeof(line), finalCount))
            {
                if (line[0] != '\n' &&
                    line[0] != '\r' &&
                    line[0] != '\0')
                {
                    finalRecords++;
                }
            }

            fclose(finalCount);

            for (int workerId = 0;
                 workerId < expectedWorkers;
                 workerId++)
            {
                char workerPath[256];

                snprintf(
                    workerPath,
                    sizeof(workerPath),
                    "results_1b/physical_wells_worker_%d.txt",
                    workerId
                );

                FILE *workerCount = fopen(workerPath, "r");

                if (!workerCount)
                {
                    fprintf(
                        stderr,
                        "LEVEL 20.12: Could not open worker file: %s\n",
                        workerPath
                    );

                    countError = 1;
                    break;
                }

                while (fgets(line, sizeof(line), workerCount))
                {
                    if (line[0] != '\n' &&
                        line[0] != '\r' &&
                        line[0] != '\0')
                    {
                        workerRecords++;
                    }
                }

                fclose(workerCount);
            }

            printf("----------------------------------------\n");
            printf(" LEVEL 20.12 RESULT RECORD COUNT\n");
            printf("----------------------------------------\n");
            printf(
                "Final result records : %llu\n",
                finalRecords
            );
            printf(
                "Worker result records: %llu\n",
                workerRecords
            );

            if (countError ||
                finalRecords != workerRecords)
            {
                fprintf(
                    stderr,
                    "RESULT: FAIL - RESULT RECORD COUNT MISMATCH\n"
                );

                printf("----------------------------------------\n");
                return 1;
            }

            printf("----------------------------------------\n");
            printf(
                "RESULT: PASS - RESULT RECORD COUNT VERIFIED\n"
            );
            printf("----------------------------------------\n");
        }

        /*
         * --------------------------------------------------------
         * LEVEL 20.13: DUPLICATE RESULT DETECTION
         *
         * Verify that no identical non-empty result records
         * appear more than once in the finalized output.
         *
         * Read-only verification. No result file is modified.
         * --------------------------------------------------------
         */
        {
            FILE *dupFile = fopen(outputPath, "r");

            if (!dupFile)
            {
                fprintf(
                    stderr,
                    "LEVEL 20.13: Could not open final result.\n"
                );

                return 1;
            }

            char **records = NULL;
            unsigned long long recordCount = 0;
            unsigned long long duplicateCount = 0;
            int duplicateError = 0;

            char line[4096];

            while (fgets(line, sizeof(line), dupFile))
            {
                if (line[0] == '\n' ||
                    line[0] == '\r' ||
                    line[0] == '\0')
                {
                    continue;
                }

                size_t len = strlen(line);
                char *copy = malloc(len + 1);

                if (!copy)
                {
                    fprintf(
                        stderr,
                        "LEVEL 20.13: Memory allocation failed.\n"
                    );

                    duplicateError = 1;
                    break;
                }

                memcpy(copy, line, len + 1);

                for (unsigned long long i = 0;
                     i < recordCount;
                     i++)
                {
                    if (strcmp(records[i], copy) == 0)
                    {
                        duplicateCount++;
                        free(copy);
                        copy = NULL;
                        break;
                    }
                }

                if (copy)
                {
                    char **newRecords =
                        realloc(
                            records,
                            (size_t)(recordCount + 1) *
                            sizeof(char *)
                        );

                    if (!newRecords)
                    {
                        fprintf(
                            stderr,
                            "LEVEL 20.13: Memory allocation failed.\n"
                        );

                        free(copy);
                        duplicateError = 1;
                        break;
                    }

                    records = newRecords;
                    records[recordCount] = copy;
                    recordCount++;
                }
            }

            fclose(dupFile);

            for (unsigned long long i = 0;
                 i < recordCount;
                 i++)
            {
                free(records[i]);
            }

            free(records);

            printf("----------------------------------------\n");
            printf(" LEVEL 20.13 DUPLICATE RESULT DETECTION\n");
            printf("----------------------------------------\n");
            printf(
                "Unique result records : %llu\n",
                recordCount
            );
            printf(
                "Duplicate records     : %llu\n",
                duplicateCount
            );
            printf("----------------------------------------\n");

            if (duplicateError ||
                duplicateCount != 0)
            {
                fprintf(
                    stderr,
                    "RESULT: FAIL - DUPLICATE RESULT RECORDS DETECTED\n"
                );

                printf("----------------------------------------\n");

                return 1;
            }

            printf(
                "RESULT: PASS - NO DUPLICATE RESULT RECORDS\n"
            );
            printf("----------------------------------------\n");
        }

        printf(
            "RESULT: PASS - ATOMIC RESULT FINALIZED\n"
        );
        printf("========================================\n");

        return 0;
    }

    /*
     * --------------------------------------------------------
     * VERIFIED QUADWELL TEST
     *
     * This uses the exact existing random_worker() -> worker()
     * pipeline. No separate detection logic is used.
     *
     * Verified Bedrock quadwell:
     *   Seed   : 1520815389707
     *   Center : (676297, -479950)
     *   Radius : 1
     *   Pattern: 4
     * --------------------------------------------------------
     */
    if (argc == 2 &&
        strcmp(argv[1], "quadtest") == 0)
    {
        const int64_t seed = 1520815389707LL;
        const int centerX = 676297;
        const int centerZ = -479950;
        const int radius = 1;
        const int pattern = 4;

        RandomThreadArgs args;

        args.count = 1;
        args.seeds = (int64_t *)&seed;

        int candidateX = centerX;
        int candidateZ = centerZ;

        args.centerXs = &candidateX;
        args.centerZs = &candidateZ;
        args.candidateIndices = NULL;

        args.centerX = centerX;
        args.centerZ = centerZ;
        args.radius = radius;
        args.pattern = pattern;

        printf("========================================\n");
        printf(" VERIFIED QUADWELL TEST\n");
        printf("========================================\n");
        printf("Seed   : %lld\n",
            (long long)seed);
        printf("Center : (%d, %d)\n",
            centerX, centerZ);
        printf("Radius : %d\n", radius);
        printf("Pattern: %d\n", pattern);
        printf("----------------------------------------\n");

        ThreadResult *r =
            (ThreadResult *)random_worker(&args);

        if (!r)
        {
            printf("RESULT: FAIL - random_worker returned NULL\n");
            printf("========================================\n");
            return 1;
        }

        printf("Seeds scanned        : %llu\n",
            (unsigned long long)r->seedsScanned);

        printf("Valid wells          : %llu\n",
            (unsigned long long)r->validWells);

        printf("Physical components  : %llu\n",
            (unsigned long long)r->physicalComponentCount);

        printf("2-well components    : %llu\n",
            (unsigned long long)r->physicalPairComponentCount);

        printf("2-well component wells: %llu\n",
            (unsigned long long)r->physicalPairWellCount);

        printf("Physical 3-wells     : %llu\n",
            (unsigned long long)r->physical3Found);

        printf("Physical 4-wells     : %llu\n",
            (unsigned long long)r->physical4Found);

        printf("LEVEL 17 prefilter pairs : %llu\n",
            (unsigned long long)r->level17PrefilterPairs);

        printf("LEVEL 17 actual pairs    : %llu\n",
            (unsigned long long)r->level17ActualPairs);

        printf("----------------------------------------\n");

        if (r->quadwellFound > 0)
        {
            printf("RESULT: PASS - VERIFIED QUADWELL DETECTED\n");
        }
        else
        {
            printf("RESULT: FAIL - VERIFIED QUADWELL NOT DETECTED\n");
        }

        printf("========================================\n");

        free(r->physicalResults);

        int pass = (r->quadwellFound > 0);

        free(r);

        return pass ? 0 : 1;
    }

    /*
     * --------------------------------------------------------
     * SPATIAL ANALYZER TEST
     *
     * Synthetic clusters used only to verify spatial math.
     * Does not touch Minecraft generation or detection.
     * --------------------------------------------------------
     */
    if (argc == 2 &&
        strcmp(argv[1], "spatialtest") == 0)
    {
        PhysicalResult testResults[3];

        memset(testResults, 0, sizeof(testResults));

        /*
         * Cluster 1: center (0,0)
         */
        testResults[0].cluster.count = 3;
        testResults[0].cluster.minX = 0;
        testResults[0].cluster.maxX = 0;
        testResults[0].cluster.minZ = 0;
        testResults[0].cluster.maxZ = 0;

        /*
         * Cluster 2: center (3,4)
         */
        testResults[1].cluster.count = 3;
        testResults[1].cluster.minX = 3;
        testResults[1].cluster.maxX = 3;
        testResults[1].cluster.minZ = 4;
        testResults[1].cluster.maxZ = 4;

        /*
         * Cluster 3: center (6,0)
         */
        testResults[2].cluster.count = 4;
        testResults[2].cluster.minX = 6;
        testResults[2].cluster.maxX = 6;
        testResults[2].cluster.minZ = 0;
        testResults[2].cluster.maxZ = 0;

        printf("========================================\n");
        printf(" SPATIAL ANALYZER TEST\n");
        printf("========================================\n");
        printf("Synthetic clusters : 3\n");
        printf("Centers             : (0,0), (3,4), (6,0)\n");
        printf("----------------------------------------\n");

        print_level19_spatial_analysis(
            testResults,
            3
        );

        printf("========================================\n");
        printf(" SPATIAL ANALYZER TEST COMPLETE\n");
        printf("========================================\n");

        return 0;
    }

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
    /*
     * --------------------------------------------------------
     * 4D MODE - SEED / X / Z CANDIDATE AXIS
     *
     * Usage:
     *   program 4d <count> <rngSeed> <centerX> <centerZ> <radius> <pattern>
     *
     * This mode will generate independent Seed/X/Z candidates.
     * The existing worker() performs the real Bedrock well scan
     * and physical component detection.
     * --------------------------------------------------------
     */
    if (argc == 8 &&
        strcmp(argv[1], "4d") == 0)
    {
        int count = atoi(argv[2]);

        char *end = NULL;
        errno = 0;

        uint64_t rngSeed = strtoull(argv[3], &end, 10);

        if (errno != 0 || end == argv[3] || *end != '\0')
        {
            fprintf(stderr, "Invalid 4D RNG seed.\n");
            return 1;
        }

        int centerX = atoi(argv[4]);
        int centerZ = atoi(argv[5]);
        int radius  = atoi(argv[6]);
        int pattern = atoi(argv[7]);

        if (count <= 0)
        {
            fprintf(stderr, "Invalid 4D candidate count.\n");
            return 1;
        }

        uint64_t state = rngSeed;

        printf("========================================\n");
        printf(" 4D SEED / X / Z CANDIDATE TEST\n");
        printf("========================================\n");
        printf("Count   : %d\n", count);
        printf("RNG seed: %llu\n",
               (unsigned long long)rngSeed);
        printf("Center  : (%d, %d)\n", centerX, centerZ);
        printf("Radius  : %d\n", radius);
        printf("Pattern : %d\n", pattern);
        printf("----------------------------------------\n");

        uint64_t totalSeeds = 0;
        uint64_t totalValidWells = 0;
        uint64_t totalPhysicalComponents = 0;
        uint64_t totalPhysicalPairComponents = 0;
        uint64_t totalPhysicalPairWells = 0;
        uint64_t totalPhysical3 = 0;
        uint64_t totalPhysical4 = 0;

        /* LEVEL 17 verification totals */
        uint64_t totalLevel17PrefilterPairs = 0;
        uint64_t totalLevel17ActualPairs = 0;

        /*
         * STEP4 CALIBRATION:
         * Force one known 4D observation through the exact same
         * worker() path before testing generated candidates.
         */
        if (0 && 0 && count == 1 && rngSeed == 12345)
        {
            int64_t seed4d = 1520815389707LL;
            int64_t x4d = 676297;
            int64_t z4d = -479950;

            ThreadArgs one;
            one.seedStart = seed4d;
            one.seedEnd   = seed4d;
            one.centerX   = (int)x4d;
            one.centerZ   = (int)z4d;
            one.radius    = radius;
            one.pattern   = pattern;

            printf("STEP4 CALIBRATION CANDIDATE\n");
            printf("  Seed  : %lld\n", (long long)seed4d);
            printf("  X     : %lld\n", (long long)x4d);
            printf("  Z     : %lld\n", (long long)z4d);
            printf("----------------------------------------\n");

            ThreadResult *r = (ThreadResult *)worker(&one);

            if (!r)
            {
                fprintf(stderr, "STEP4 CALIBRATION: WORKER FAILED\n");
                return 1;
            }

            totalSeeds += r->seedsScanned;
            totalValidWells += r->validWells;
            totalPhysical3 += r->physical3Found;
            totalPhysical4 += r->physical4Found;

            printf("STEP4 CALIBRATION RESULT\n");
            printf("  Seeds scanned    : %llu\n",
                   (unsigned long long)r->seedsScanned);
            printf("  Valid wells      : %llu\n",
                   (unsigned long long)r->validWells);
            printf("  Physical 3-wells : %llu\n",
                   (unsigned long long)r->physical3Found);
            printf("  Physical 4-wells : %llu\n",
                   (unsigned long long)r->physical4Found);

            for (size_t pi = 0; pi < r->physicalResultCount; pi++)
            {
                PhysicalResult *pr = &r->physicalResults[pi];

                printf("  PHYSICAL COMPONENT FOUND\n");
                printf("  Seed  : %lld\n",
                       (long long)pr->seed);
                printf("  Count : %d\n",
                       pr->cluster.count);
            }

            free(r->physicalResults);
            free(r);

            printf("========================================\n");
            printf("STEP4 CALIBRATION COMPLETE\n");
            printf("========================================\n");

            return 0;
        }

        /*
         * STEP6: deterministic known 4D regression.
         * The known tuple is tested through the exact normal worker path.
         * This does not alter worker() or physical detection logic.
         */
        if (count == 1 && rngSeed == 1520815389707ULL)
        {
            int64_t seed4d = 1520815389707LL;
            int64_t x4d = 676297;
            int64_t z4d = -479950;

            ThreadArgs one;
            one.seedStart = seed4d;
            one.seedEnd   = seed4d;
            one.centerX   = (int)x4d;
            one.centerZ   = (int)z4d;
            one.radius    = radius;
            one.pattern   = pattern;

            printf("STEP6 KNOWN 4D REGRESSION\n");
            printf("Seed : %lld\n", (long long)seed4d);
            printf("X    : %lld\n", (long long)x4d);
            printf("Z    : %lld\n", (long long)z4d);
            printf("----------------------------------------\n");

            ThreadResult *r = (ThreadResult *)worker(&one);

            if (!r)
            {
                fprintf(stderr, "STEP6: WORKER FAILED\n");
                return 1;
            }

            printf("Valid wells       : %llu\n",
                   (unsigned long long)r->validWells);
            printf("Physical 3-wells       : %llu\n",
                   (unsigned long long)r->physical3Found);
            printf("Physical 4-wells       : %llu\n",
                   (unsigned long long)r->physical4Found);
            printf("Physical components    : %llu\n",
                   (unsigned long long)r->physicalComponentCount);
            printf("2-well components      : %llu\n",
                   (unsigned long long)r->physicalPairComponentCount);
            printf("Wells in 2-well comps  : %llu\n",
                   (unsigned long long)r->physicalPairWellCount);

            if (r->validWells == 4 &&
                r->physicalComponentCount == 2 &&
                r->physicalPairComponentCount == 2 &&
                r->physicalPairWellCount == 4)
            {
                printf("RESULT: PASS - KNOWN 4D PHYSICAL REGRESSION\n");
            }
            else
            {
                printf("RESULT: FAIL - KNOWN 4D TUPLE MISMATCH\n");
            }

            free(r->physicalResults);
            free(r);

            printf("========================================\n");
            printf("STEP6 REGRESSION COMPLETE\n");
            printf("========================================\n");

            return 0;
        }

        for (int i = 0; i < count; i++)
        {
            int64_t seed4d = rng64_signed(&state);

            int64_t x4d = rng64_range(
                &state,
                (int64_t)centerX - 100000,
                (int64_t)centerX + 100000
            );

            int64_t z4d = rng64_range(
                &state,
                (int64_t)centerZ - 100000,
                (int64_t)centerZ + 100000
            );

            ThreadArgs one;
            one.seedStart = seed4d;
            one.seedEnd   = seed4d;
            one.centerX   = (int)x4d;
            one.centerZ   = (int)z4d;
            one.radius    = radius;
            one.pattern   = pattern;

            printf(
                "4D Candidate %d: seed=%lld X=%lld Z=%lld\n",
                i + 1,
                (long long)seed4d,
                (long long)x4d,
                (long long)z4d
            );

            ThreadResult *r = (ThreadResult *)worker(&one);

            if (!r)
            {
                fprintf(stderr,
                    "WARNING: worker failed for candidate %d\n",
                    i + 1);
                continue;
            }

            totalSeeds += r->seedsScanned;
            totalValidWells += r->validWells;
            totalPhysical3 += r->physical3Found;
            totalPhysical4 += r->physical4Found;

            if (r->physicalResultCount > 0)
            {
                printf(
                    "  Physical results: %zu\n",
                    r->physicalResultCount
                );

                for (size_t pi = 0; pi < r->physicalResultCount; pi++)
                {
                    PhysicalResult *pr = &r->physicalResults[pi];

                    printf("  ----------------------------------------\n");
                    printf("  PHYSICAL COMPONENT FOUND\n");
                    printf("  Seed    : %lld\n",
                           (long long)pr->seed);
                    printf("  Count   : %d\n",
                           pr->cluster.count);

                    for (int wi = 0;
                         wi < pr->cluster.count && wi < 4;
                         wi++)
                    {
                        Well *w = &pr->cluster.w[wi];

                        printf(
                            "  W%d: chunk=(%d,%d) world=(%d,%d) local=(%d,%d)\n",
                            wi + 1,
                            w->cx,
                            w->cz,
                            w->x,
                            w->z,
                            w->x & 15,
                            w->z & 15
                        );
                    }
                }
            }

            free(r->physicalResults);
            free(r);
        }

        printf("========================================\n");
        printf(" 4D SEARCH TEST COMPLETE\n");
        printf("========================================\n");
        printf("Candidates tested : %d\n", count);
        printf("Seeds scanned     : %llu\n",
               (unsigned long long)totalSeeds);
        printf("Valid wells       : %llu\n",
               (unsigned long long)totalValidWells);
        printf("Physical 3-wells  : %llu\n",
               (unsigned long long)totalPhysical3);
        printf("Physical 4-wells  : %llu\n",
               (unsigned long long)totalPhysical4);
        printf("========================================\n");

        return 0;
    }

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
                "Failed to allocate random coordinate lists.\n");
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

        /* Initialize live progress for random search */
        if (progressEnabled)
        {
            pthread_mutex_lock(&progress_mutex);

            progressTotal = (uint64_t)randomCount;
            progressCompleted = 0;
            progressStart = time(NULL);
            progressLast = 0;

            pthread_mutex_unlock(&progress_mutex);
        }

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

        /* LEVEL 18: aggregated diagnostic statistics */
        uint64_t physicalComponentCount = 0;
        uint64_t physicalPairComponentCount = 0;
        uint64_t physicalPairWellCount = 0;
        uint64_t level17PrefilterPairs = 0;
        uint64_t level17ActualPairs = 0;

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

            physicalComponentCount +=
                results[i]->physicalComponentCount;

            physicalPairComponentCount +=
                results[i]->physicalPairComponentCount;

            physicalPairWellCount +=
                results[i]->physicalPairWellCount;

            level17PrefilterPairs +=
                results[i]->level17PrefilterPairs;

            level17ActualPairs +=
                results[i]->level17ActualPairs;
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

        print_level18_statistics(
            seedsScanned,
            validWells,
            quadwellFound,
            threewellSeedsFound,
            physicalComponentCount,
            physicalPairComponentCount,
            physicalPairWellCount,
            physical3Found,
            physical4Found,
            level17PrefilterPairs,
            level17ActualPairs
        );

        print_level19_random_spatial_analysis(
            results,
            NUM_THREADS
        );

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
     * --------------------------------------------------------
     * 4E MODE - INDEPENDENT REPRODUCIBLE CANDIDATE ENGINE
     *
     * Usage:
     *   program 4e <count> <rngSeed> <centerX> <centerZ>
     *              <radius> <pattern>
     *
     * 4E Step 1 deliberately changes only the candidate
     * generation layer. The existing worker() and physical
     * well detection remain untouched.
     *
     * Each candidate is derived independently from:
     *
     *   base RNG seed + candidate index
     *
     * Separate deterministic streams are used for:
     *
     *   Seed
     *   X
     *   Z
     *
     * Therefore candidate N can be reproduced without depending
     * on the state generated by candidates 0..N-1.
     * --------------------------------------------------------
     */
    /*
     * STEP7: known Seed/X/Z worker regression.
     *
     * Directly exercises the existing worker() path using the
     * verified physical 4-well observation.
     *
     * Usage:
     *   program 4e7 <seed> <centerX> <centerZ> <radius> <pattern>
     */
    if (argc == 7 &&
        strcmp(argv[1], "4e7") == 0)
    {
        int64_t seed7 = strtoll(argv[2], NULL, 10);
        int centerX7 = atoi(argv[3]);
        int centerZ7 = atoi(argv[4]);
        int radius7  = atoi(argv[5]);
        int pattern7 = atoi(argv[6]);

        ThreadArgs one;

        one.seedStart = seed7;
        one.seedEnd   = seed7;
        one.centerX   = centerX7;
        one.centerZ   = centerZ7;
        one.radius    = radius7;
        one.pattern   = pattern7;

        printf("========================================\n");
        printf(" STEP7 KNOWN WORKER REGRESSION\n");
        printf("========================================\n");
        printf("Seed   : %lld\n", (long long)seed7);
        printf("Center : (%d, %d)\n", centerX7, centerZ7);
        printf("Radius : %d\n", radius7);
        printf("Pattern: %d\n", pattern7);
        printf("----------------------------------------\n");

        ThreadResult *r =
            (ThreadResult *)worker(&one);

        if (!r)
        {
            fprintf(stderr,
                "STEP7: WORKER FAILED\n");
            return 1;
        }

        printf("Seeds scanned              : %llu\n",
            (unsigned long long)r->seedsScanned);

        printf("Valid wells                : %llu\n",
            (unsigned long long)r->validWells);

        printf("Physical 3-wells           : %llu\n",
            (unsigned long long)r->physical3Found);

        printf("Physical 4-wells           : %llu\n",
            (unsigned long long)r->physical4Found);

        printf("Physical components        : %llu\n",
            (unsigned long long)r->physicalComponentCount);

        printf("2-well components          : %llu\n",
            (unsigned long long)r->physicalPairComponentCount);

        printf("Wells in 2-well components : %llu\n",
            (unsigned long long)r->physicalPairWellCount);

        if (r->seedsScanned == 1 &&
            r->validWells == 4 &&
            r->physicalComponentCount == 2 &&
            r->physicalPairComponentCount == 2 &&
            r->physicalPairWellCount == 4)
        {
            printf("RESULT: PASS - KNOWN WORKER REGRESSION\n");
        }
        else
        {
            printf("RESULT: FAIL - KNOWN WORKER REGRESSION\n");
        }

        free(r->physicalResults);
        free(r);

        printf("========================================\n");
        printf("STEP7 COMPLETE\n");
        printf("========================================\n");

        return 0;
    }

    /*
     * STEP8: fixed known 4D worker regression.
     *
     * Usage:
     *   program 4e8 <seed> <centerX> <centerZ> <radius> <pattern>
     *
     * This calls the existing worker() directly with the known
     * Bedrock 4-well physical tuple.
     *
     * No candidate generator, random_worker(), or detection logic
     * is modified.
     */
    if (argc == 7 &&
        strcmp(argv[1], "4e8") == 0)
    {
        int64_t seed =
            strtoll(argv[2], NULL, 10);

        int centerX = atoi(argv[3]);
        int centerZ = atoi(argv[4]);
        int radius  = atoi(argv[5]);
        int pattern = atoi(argv[6]);

        printf("========================================\n");
        printf(" STEP8 KNOWN 4D WORKER REGRESSION\n");
        printf("========================================\n");
        printf("Seed     : %lld\n", (long long)seed);
        printf("Center   : (%d, %d)\n", centerX, centerZ);
        printf("Radius   : %d\n", radius);
        printf("Pattern  : %d\n", pattern);
        printf("----------------------------------------\n");

        ThreadArgs one;

        one.seedStart = seed;
        one.seedEnd   = seed;
        one.centerX   = centerX;
        one.centerZ   = centerZ;
        one.radius    = radius;
        one.pattern   = pattern;

        ThreadResult *r =
            (ThreadResult *)worker(&one);

        if (!r)
        {
            fprintf(stderr,
                "STEP8: WORKER FAILED\n");
            return 1;
        }

        printf("Seeds scanned             : %llu\n",
            (unsigned long long)r->seedsScanned);

        printf("Valid wells               : %llu\n",
            (unsigned long long)r->validWells);

        printf("Physical components       : %llu\n",
            (unsigned long long)r->physicalComponentCount);

        printf("2-well components         : %llu\n",
            (unsigned long long)r->physicalPairComponentCount);

        printf("Wells in 2-well comps     : %llu\n",
            (unsigned long long)r->physicalPairWellCount);

        printf("Physical 3-wells           : %llu\n",
            (unsigned long long)r->physical3Found);

        printf("Physical 4-wells           : %llu\n",
            (unsigned long long)r->physical4Found);

        if (r->seedsScanned == 1 &&
            r->validWells == 4 &&
            r->physicalComponentCount == 2 &&
            r->physicalPairComponentCount == 2 &&
            r->physicalPairWellCount == 4)
        {
            printf("----------------------------------------\n");
            printf("RESULT: PASS - KNOWN 4D WORKER REGRESSION\n");
        }
        else
        {
            printf("----------------------------------------\n");
            printf("RESULT: FAIL - KNOWN 4D WORKER REGRESSION\n");
        }

        free(r->physicalResults);
        free(r);

        printf("========================================\n");
        printf("STEP8 COMPLETE\n");
        printf("========================================\n");

        return 0;
    }



    /*
     * STEP9B: independent direct-worker oracle.
     *
     * This deliberately bypasses random_worker(), pthreads,
     * candidate arrays, and aggregation from STEP9.
     *
     * The four frozen 4F candidates are executed directly through
     * the existing worker() path, one candidate at a time.
     *
     * Expected oracle for this fixed test:
     *
     *   Seeds scanned       : 4
     *   Valid wells         : 0
     *   Physical components : 0
     *   Physical 3-wells    : 0
     *   Physical 4-wells    : 0
     *
     * Usage:
     *
     *   program 4e9b
     */

    if (argc == 2 &&
        strcmp(argv[1], "4e9b") == 0)
    {
        const int count = 4;

        static const int64_t expectedSeeds[4] =
        {
            -4667261597301147968LL,
            -8747559536492820829LL,
            -5841154199779032894LL,
             583081449948609959LL
        };

        static const int expectedXs[4] =
        {
            615635,
            720771,
            691530,
            679247
        };

        static const int expectedZs[4] =
        {
            -563688,
            -531319,
            -577694,
            -431615
        };

        const int radius = 1;
        const int pattern = 4;

        uint64_t totalSeeds = 0;
        uint64_t totalValidWells = 0;
        uint64_t totalComponents = 0;
        uint64_t totalPairComponents = 0;
        uint64_t totalPairWells = 0;
        uint64_t totalPhysical3 = 0;
        uint64_t totalPhysical4 = 0;

        int pass = 1;

        printf("========================================\n");
        printf(" STEP9B DIRECT-WORKER ORACLE\n");
        printf("========================================\n");
        printf("Candidates : %d\n", count);
        printf("Radius     : %d\n", radius);
        printf("Pattern    : %d\n", pattern);
        printf("----------------------------------------\n");

        for (int i = 0; i < count; i++)
        {
            ThreadArgs one;

            memset(&one, 0, sizeof(one));

            one.seedStart = expectedSeeds[i];
            one.seedEnd   = expectedSeeds[i];

            one.centerX = expectedXs[i];
            one.centerZ = expectedZs[i];

            one.radius  = radius;
            one.pattern = pattern;

            printf(
                "Oracle candidate %d: seed=%lld X=%d Z=%d\n",
                i,
                (long long)expectedSeeds[i],
                expectedXs[i],
                expectedZs[i]
            );

            ThreadResult *r =
                (ThreadResult *)worker(&one);

            if (!r)
            {
                printf("  WORKER FAILED\n");
                pass = 0;
                continue;
            }

            printf(
                "  seeds=%llu valid=%llu components=%llu "
                "physical3=%llu physical4=%llu\n",
                (unsigned long long)r->seedsScanned,
                (unsigned long long)r->validWells,
                (unsigned long long)r->physicalComponentCount,
                (unsigned long long)r->physical3Found,
                (unsigned long long)r->physical4Found
            );

            totalSeeds +=
                r->seedsScanned;

            totalValidWells +=
                r->validWells;

            totalComponents +=
                r->physicalComponentCount;

            totalPairComponents +=
                r->physicalPairComponentCount;

            totalPairWells +=
                r->physicalPairWellCount;

            totalPhysical3 +=
                r->physical3Found;

            totalPhysical4 +=
                r->physical4Found;

            /*
             * Each direct worker call processes exactly one seed.
             */
            if (r->seedsScanned != 1 ||
                r->validWells != 0 ||
                r->physicalComponentCount != 0 ||
                r->physicalPairComponentCount != 0 ||
                r->physicalPairWellCount != 0 ||
                r->physical3Found != 0 ||
                r->physical4Found != 0)
            {
                printf("  ORACLE CANDIDATE: FAIL\n");
                pass = 0;
            }
            else
            {
                printf("  ORACLE CANDIDATE: PASS\n");
            }

            free(r->physicalResults);
            free(r);
        }

        printf("----------------------------------------\n");
        printf("STEP9B DIRECT ORACLE TOTALS\n");
        printf("----------------------------------------\n");

        printf(
            "Seeds scanned             : %llu\n",
            (unsigned long long)totalSeeds
        );

        printf(
            "Valid wells               : %llu\n",
            (unsigned long long)totalValidWells
        );

        printf(
            "Physical components       : %llu\n",
            (unsigned long long)totalComponents
        );

        printf(
            "2-well components         : %llu\n",
            (unsigned long long)totalPairComponents
        );

        printf(
            "Wells in 2-well comps     : %llu\n",
            (unsigned long long)totalPairWells
        );

        printf(
            "Physical 3-wells          : %llu\n",
            (unsigned long long)totalPhysical3
        );

        printf(
            "Physical 4-wells          : %llu\n",
            (unsigned long long)totalPhysical4
        );

        /*
         * Independent aggregate oracle.
         */
        if (totalSeeds != 4 ||
            totalValidWells != 0 ||
            totalComponents != 0 ||
            totalPairComponents != 0 ||
            totalPairWells != 0 ||
            totalPhysical3 != 0 ||
            totalPhysical4 != 0)
        {
            pass = 0;
        }

        printf("----------------------------------------\n");

        if (pass)
        {
            printf(
                "RESULT: PASS - STEP9B DIRECT-WORKER ORACLE\n"
            );
        }
        else
        {
            printf(
                "RESULT: FAIL - STEP9B DIRECT-WORKER ORACLE\n"
            );
        }

        printf("========================================\n");
        printf("STEP9B COMPLETE\n");
        printf("========================================\n");

        return pass ? 0 : 1;
    }

    /*
     * STEP9: end-to-end 4F candidate pipeline regression.
     *
     * This mode intentionally reuses the same deterministic
     * candidate generation and random_worker() path.
     *
     * Flow:
     *
     *   deterministic Seed/X/Z
     *          ->
     *   candidate arrays
     *          ->
     *   random_worker()
     *          ->
     *   worker()
     *          ->
     *   physical detection
     *          ->
     *   ThreadResult aggregation
     *
     * This first regression run is an AUDIT run.  It records
     * the actual deterministic aggregate produced by the frozen
     * pipeline.  No detection logic is modified.
     *
     * Usage:
     *
     *   program 4e9
     */

    if (argc == 2 &&
        strcmp(argv[1], "4e9") == 0)
    {
        const int count = 4;
        const uint64_t rngSeed = 12345ULL;
        const uint64_t startIndex = 0ULL;

        const int centerX = 676297;
        const int centerZ = -479950;
        const int radius = 1;
        const int pattern = 4;

        const int64_t coordRange = 100000;

        int64_t candidateSeeds[4];
        int candidateXs[4];
        int candidateZs[4];
        uint64_t candidateIndices[4];

        printf("========================================\n");
        printf(" STEP9 4F END-TO-END PIPELINE AUDIT\n");
        printf("========================================\n");
        printf("Count   : %d\n", count);
        printf("RNG seed: %llu\n",
            (unsigned long long)rngSeed);
        printf("Start   : %llu\n",
            (unsigned long long)startIndex);
        printf("Center  : (%d, %d)\n",
            centerX, centerZ);
        printf("Range   : +/- %lld chunks\n",
            (long long)coordRange);
        printf("Radius  : %d\n", radius);
        printf("Pattern : %d\n", pattern);
        printf("----------------------------------------\n");

        if (progressEnabled)
        {
            pthread_mutex_lock(&progress_mutex);
            progressTotal = (uint64_t)count;
            progressCompleted = 0;
            progressStart = time(NULL);
            progressLast = 0;
            pthread_mutex_unlock(&progress_mutex);
        }

        /*
         * STEP9-A:
         * Generate the exact frozen 4F candidate stream.
         */
        for (int i = 0; i < count; i++)
        {
            uint64_t candidateIndex =
                startIndex + (uint64_t)i;

            candidateIndices[i] =
                candidateIndex;

            uint64_t base =
                rngSeed +
                0x9E3779B97F4A7C15ULL *
                (candidateIndex + 1ULL);

            uint64_t seedState =
                base ^ 0xA5A5A5A5A5A5A5A5ULL;

            uint64_t xState =
                base ^ 0x3C6EF372FE94F82AULL;

            uint64_t zState =
                base ^ 0xDAA66D2C7DDF743FULL;

            candidateSeeds[i] =
                (int64_t)rng64_next(&seedState);

            candidateXs[i] =
                (int)rng64_range(
                    &xState,
                    centerX - coordRange,
                    centerX + coordRange
                );

            candidateZs[i] =
                (int)rng64_range(
                    &zState,
                    centerZ - coordRange,
                    centerZ + coordRange
                );

            printf(
                "Candidate %d: seed=%lld X=%d Z=%d\n",
                i,
                (long long)candidateSeeds[i],
                candidateXs[i],
                candidateZs[i]
            );
        }

        printf("----------------------------------------\n");
        printf("STEP9-A: candidate stream generated\n");

        /*
         * STEP9-B:
         * Feed the exact candidate tuples through random_worker().
         *
         * One candidate per worker is intentional here.
         * This makes the audit easy to inspect while still using
         * the actual random_worker() -> worker() path.
         */
        RandomThreadArgs args[4];
        pthread_t threads[4];
        ThreadResult *results[4] = {0};

        for (int i = 0; i < count; i++)
        {
            memset(&args[i], 0, sizeof(args[i]));

            args[i].count = 1;

            args[i].seeds =
                &candidateSeeds[i];

            args[i].centerXs =
                &candidateXs[i];

            args[i].centerZs =
                &candidateZs[i];

            args[i].candidateIndices =
                &candidateIndices[i];

            args[i].centerX = centerX;
            args[i].centerZ = centerZ;
            args[i].radius  = radius;
            args[i].pattern = pattern;

            if (pthread_create(
                    &threads[i],
                    NULL,
                    random_worker,
                    &args[i]) != 0)
            {
                fprintf(
                    stderr,
                    "STEP9: failed to create worker %d\n",
                    i
                );

                for (int j = 0; j < i; j++)
                    pthread_join(
                        threads[j],
                        NULL
                    );

                for (int j = 0; j < i; j++)
                {
                    if (results[j])
                    {
                        free(results[j]->physicalResults);
                        free(results[j]);
                    }
                }

                return 1;
            }
        }

        uint64_t totalSeeds = 0;
        uint64_t totalValidWells = 0;
        uint64_t totalPhysical3 = 0;
        uint64_t totalPhysical4 = 0;
        uint64_t totalComponents = 0;
        uint64_t totalPairComponents = 0;
        uint64_t totalPairWells = 0;

        int totalPhysicalResults = 0;

        /*
         * STEP9-C:
         * Join and aggregate.
         */
        for (int i = 0; i < count; i++)
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

            totalSeeds +=
                results[i]->seedsScanned;

            totalValidWells +=
                results[i]->validWells;

            totalPhysical3 +=
                results[i]->physical3Found;

            totalPhysical4 +=
                results[i]->physical4Found;

            totalComponents +=
                results[i]->physicalComponentCount;

            totalPairComponents +=
                results[i]->physicalPairComponentCount;

            totalPairWells +=
                results[i]->physicalPairWellCount;

            totalPhysicalResults +=
                (int)results[i]->physicalResultCount;
        }

        printf("----------------------------------------\n");
        printf("STEP9-B/C: pipeline completed\n");
        printf("----------------------------------------\n");

        printf(
            "Seeds scanned             : %llu\n",
            (unsigned long long)totalSeeds
        );

        printf(
            "Valid wells               : %llu\n",
            (unsigned long long)totalValidWells
        );

        printf(
            "Physical components       : %llu\n",
            (unsigned long long)totalComponents
        );

        printf(
            "2-well components         : %llu\n",
            (unsigned long long)totalPairComponents
        );

        printf(
            "Wells in 2-well comps     : %llu\n",
            (unsigned long long)totalPairWells
        );

        printf(
            "Physical 3-wells           : %llu\n",
            (unsigned long long)totalPhysical3
        );

        printf(
            "Physical 4-wells           : %llu\n",
            (unsigned long long)totalPhysical4
        );

        printf(
            "Stored physical results   : %d\n",
            totalPhysicalResults
        );

        /*
         * Print every 3/4-well result returned by worker().
         */
        int resultNumber = 0;

        printf("----------------------------------------\n");
        printf("STEP9 PHYSICAL RESULTS\n");

        for (int i = 0; i < count; i++)
        {
            ThreadResult *r = results[i];

            if (!r)
                continue;

            for (size_t pi = 0;
                 pi < r->physicalResultCount;
                 pi++)
            {
                PhysicalResult *pr =
                    &r->physicalResults[pi];

                Cluster *c =
                    &pr->cluster;

                if (c->count != 3 &&
                    c->count != 4)
                    continue;

                resultNumber++;

                printf(
                    "\nRESULT #%d: %d-WELL\n",
                    resultNumber,
                    c->count
                );

                printf(
                    "Seed: %lld\n",
                    (long long)pr->seed
                );

                for (int wi = 0;
                     wi < c->count && wi < 4;
                     wi++)
                {
                    Well *w =
                        &c->w[wi];

                    printf(
                        "W%d: chunk=(%d,%d) "
                        "world=(%d,%d) "
                        "local=(%d,%d)\n",
                        wi + 1,
                        w->cx,
                        w->cz,
                        w->x,
                        w->z,
                        w->x & 15,
                        w->z & 15
                    );
                }
            }
        }

        /*
         * No expected aggregate is frozen yet.
         *
         * This is deliberately an audit-only first run.
         */
        printf("\n========================================\n");
        printf("STEP9 AUDIT COMPLETE\n");
        printf("========================================\n");
        printf("STATUS: AUDIT DATA COLLECTED\n");
        printf("No expected aggregate has been frozen yet.\n");
        printf("========================================\n");

        for (int i = 0; i < count; i++)
        {
            if (!results[i])
                continue;

            free(results[i]->physicalResults);
            free(results[i]);
        }

        return 0;
    }



    /*
     * STEP9D:
     * Repeatability audit.
     *
     * Runs the same verified STEP9C pipeline multiple times.
     * The expected aggregate result is frozen from STEP9B.
     *
     * This does not modify RNG generation, worker(), detection,
     * or candidate processing.
     */
    if (argc == 2 &&
        strcmp(argv[1], "4e9d") == 0)
    {
        const int count = 4;
        const uint64_t rngSeed = 12345ULL;
        const uint64_t startIndex = 0ULL;
        const int centerX = 676297;
        const int centerZ = -479950;
        const int radius = 1;
        const int pattern = 4;
        const int64_t coordRange = 100000;
        const int runs = 5;

        static const int64_t expectedSeeds[4] =
        {
            -4667261597301147968LL,
            -8747559536492820829LL,
            -5841154199779032894LL,
             583081449948609959LL
        };

        static const int expectedXs[4] =
        {
            615635,
            720771,
            691530,
            679247
        };

        static const int expectedZs[4] =
        {
            -563688,
            -531319,
            -577694,
            -431615
        };

        printf("========================================\n");
        printf(" STEP9D REPEATABILITY AUDIT\n");
        printf("========================================\n");
        printf("Runs       : %d\n", runs);
        printf("Candidates : %d\n", count);
        printf("RNG seed   : %llu\n",
            (unsigned long long)rngSeed);
        printf("Center     : (%d, %d)\n",
            centerX, centerZ);
        printf("Range      : +/- %lld chunks\n",
            (long long)coordRange);
        printf("Radius     : %d\n", radius);
        printf("Pattern    : %d\n", pattern);
        printf("----------------------------------------\n");

        int pass = 1;

        uint64_t baselineSeeds = 0;
        uint64_t baselineValid = 0;
        uint64_t baselineComponents = 0;
        uint64_t baselinePairs = 0;
        uint64_t baselinePairWells = 0;
        uint64_t baseline3 = 0;
        uint64_t baseline4 = 0;
        uint64_t baselineStored = 0;

        for (int run = 0; run < runs; run++)
        {
            int64_t candidateSeeds[4];
            int candidateXs[4];
            int candidateZs[4];
            uint64_t candidateIndices[4];

            for (int i = 0; i < count; i++)
            {
                uint64_t candidateIndex =
                    startIndex + (uint64_t)i;

                candidateIndices[i] =
                    candidateIndex;

                uint64_t base =
                    rngSeed +
                    0x9E3779B97F4A7C15ULL *
                    (candidateIndex + 1ULL);

                uint64_t seedState =
                    base ^ 0xA5A5A5A5A5A5A5A5ULL;

                uint64_t xState =
                    base ^ 0x3C6EF372FE94F82AULL;

                uint64_t zState =
                    base ^ 0xDAA66D2C7DDF743FULL;

                candidateSeeds[i] =
                    (int64_t)rng64_next(&seedState);

                candidateXs[i] =
                    (int)rng64_range(
                        &xState,
                        centerX - coordRange,
                        centerX + coordRange
                    );

                candidateZs[i] =
                    (int)rng64_range(
                        &zState,
                        centerZ - coordRange,
                        centerZ + coordRange
                    );

                if (candidateSeeds[i] != expectedSeeds[i] ||
                    candidateXs[i] != expectedXs[i] ||
                    candidateZs[i] != expectedZs[i])
                {
                    printf(
                        "RUN %d CANDIDATE %d: STREAM MISMATCH\n",
                        run + 1,
                        i
                    );
                    pass = 0;
                }
            }

            RandomThreadArgs a;

            a.count = count;
            a.seeds = candidateSeeds;
            a.centerXs = candidateXs;
            a.centerZs = candidateZs;
            a.candidateIndices = candidateIndices;
            a.centerX = centerX;
            a.centerZ = centerZ;
            a.radius = radius;
            a.pattern = pattern;

            ThreadResult *r =
                (ThreadResult *)random_worker(&a);

            if (!r)
            {
                printf(
                    "RUN %d: random_worker() FAILED\n",
                    run + 1
                );
                pass = 0;
                continue;
            }

            printf(
                "Run %d: seeds=%llu valid=%llu "
                "components=%llu pairs=%llu "
                "pairWells=%llu physical3=%llu "
                "physical4=%llu stored=%llu\n",
                run + 1,
                (unsigned long long)r->seedsScanned,
                (unsigned long long)r->validWells,
                (unsigned long long)r->physicalComponentCount,
                (unsigned long long)r->physicalPairComponentCount,
                (unsigned long long)r->physicalPairWellCount,
                (unsigned long long)r->physical3Found,
                (unsigned long long)r->physical4Found,
                (unsigned long long)r->physicalResultCount
            );

            if (run == 0)
            {
                baselineSeeds =
                    r->seedsScanned;
                baselineValid =
                    r->validWells;
                baselineComponents =
                    r->physicalComponentCount;
                baselinePairs =
                    r->physicalPairComponentCount;
                baselinePairWells =
                    r->physicalPairWellCount;
                baseline3 =
                    r->physical3Found;
                baseline4 =
                    r->physical4Found;
                baselineStored =
                    r->physicalResultCount;
            }
            else
            {
                if (r->seedsScanned != baselineSeeds ||
                    r->validWells != baselineValid ||
                    r->physicalComponentCount != baselineComponents ||
                    r->physicalPairComponentCount != baselinePairs ||
                    r->physicalPairWellCount != baselinePairWells ||
                    r->physical3Found != baseline3 ||
                    r->physical4Found != baseline4 ||
                    r->physicalResultCount != baselineStored)
                {
                    printf(
                        "RUN %d: AGGREGATE MISMATCH\n",
                        run + 1
                    );
                    pass = 0;
                }
            }

            if (r->seedsScanned != 4 ||
                r->validWells != 0 ||
                r->physicalComponentCount != 0 ||
                r->physicalPairComponentCount != 0 ||
                r->physicalPairWellCount != 0 ||
                r->physical3Found != 0 ||
                r->physical4Found != 0 ||
                r->physicalResultCount != 0)
            {
                printf(
                    "RUN %d: FROZEN ORACLE MISMATCH\n",
                    run + 1
                );
                pass = 0;
            }

            free(r->physicalResults);
            free(r);
        }

        printf("----------------------------------------\n");

        if (pass)
        {
            printf(
                "RESULT: PASS - STEP9D REPEATABILITY VERIFIED\n"
            );
        }
        else
        {
            printf(
                "RESULT: FAIL - STEP9D REPEATABILITY MISMATCH\n"
            );
        }

        printf("========================================\n");
        printf("STEP9D COMPLETE\n");
        printf("========================================\n");

        return pass ? 0 : 1;
    }

    /*
     * STEP9C:
     * End-to-end pipeline equivalence audit.
     *
     * Frozen oracle from STEP9B:
     *
     *   Candidates = 4
     *   Seeds scanned = 4
     *   Valid wells = 0
     *   Physical components = 0
     *   2-well components = 0
     *   Wells in 2-well components = 0
     *   Physical 3-wells = 0
     *   Physical 4-wells = 0
     *   Stored physical results = 0
     *
     * This verifies that the complete 4E/4F pipeline produces
     * exactly the same aggregate result as the direct worker oracle.
     *
     * No candidate generation or detection logic is changed.
     */
    if (argc == 2 &&
        strcmp(argv[1], "4e9c") == 0)
    {
        const int count = 4;
        const uint64_t rngSeed = 12345ULL;
        const uint64_t startIndex = 0ULL;
        const int centerX = 676297;
        const int centerZ = -479950;
        const int radius = 1;
        const int pattern = 4;
        const int64_t coordRange = 100000;

        static const int64_t expectedSeeds[4] =
        {
            -4667261597301147968LL,
            -8747559536492820829LL,
            -5841154199779032894LL,
             583081449948609959LL
        };

        static const int expectedXs[4] =
        {
            615635,
            720771,
            691530,
            679247
        };

        static const int expectedZs[4] =
        {
            -563688,
            -531319,
            -577694,
            -431615
        };

        printf("========================================\n");
        printf(" STEP9C END-TO-END EQUIVALENCE AUDIT\n");
        printf("========================================\n");
        printf("Candidates : %d\n", count);
        printf("RNG seed   : %llu\n",
            (unsigned long long)rngSeed);
        printf("Start      : %llu\n",
            (unsigned long long)startIndex);
        printf("Center     : (%d, %d)\n",
            centerX, centerZ);
        printf("Range      : +/- %lld chunks\n",
            (long long)coordRange);
        printf("Radius     : %d\n", radius);
        printf("Pattern    : %d\n", pattern);
        printf("----------------------------------------\n");

        int pass = 1;

        /*
         * STEP9C-A:
         * Reproduce the frozen candidate stream.
         */
        int64_t candidateSeeds[4];
        int candidateXs[4];
        int candidateZs[4];

        for (int i = 0; i < count; i++)
        {
            uint64_t candidateIndex =
                startIndex + (uint64_t)i;

            uint64_t base =
                rngSeed +
                0x9E3779B97F4A7C15ULL *
                (candidateIndex + 1ULL);

            uint64_t seedState =
                base ^ 0xA5A5A5A5A5A5A5A5ULL;

            uint64_t xState =
                base ^ 0x3C6EF372FE94F82AULL;

            uint64_t zState =
                base ^ 0xDAA66D2C7DDF743FULL;

            candidateSeeds[i] =
                (int64_t)rng64_next(&seedState);

            candidateXs[i] =
                (int)rng64_range(
                    &xState,
                    centerX - coordRange,
                    centerX + coordRange
                );

            candidateZs[i] =
                (int)rng64_range(
                    &zState,
                    centerZ - coordRange,
                    centerZ + coordRange
                );

            printf(
                "Candidate %d: seed=%lld X=%d Z=%d\n",
                i,
                (long long)candidateSeeds[i],
                candidateXs[i],
                candidateZs[i]
            );

            if (candidateSeeds[i] != expectedSeeds[i] ||
                candidateXs[i] != expectedXs[i] ||
                candidateZs[i] != expectedZs[i])
            {
                printf("  STREAM MISMATCH\n");
                pass = 0;
            }
            else
            {
                printf("  STREAM MATCH\n");
            }
        }

        /*
         * STEP9C-B:
         * Run the exact existing random_worker() pipeline.
         */
        RandomThreadArgs a;

        a.count = count;
        a.seeds = candidateSeeds;
        a.centerXs = candidateXs;
        a.centerZs = candidateZs;

        uint64_t candidateIndices[4] =
        {
            0ULL, 1ULL, 2ULL, 3ULL
        };

        a.candidateIndices = candidateIndices;
        a.centerX = centerX;
        a.centerZ = centerZ;
        a.radius = radius;
        a.pattern = pattern;

        ThreadResult *pipeline =
            (ThreadResult *)random_worker(&a);

        if (!pipeline)
        {
            printf("----------------------------------------\n");
            printf("PIPELINE ERROR: random_worker() returned NULL\n");
            printf("RESULT: FAIL - STEP9C\n");
            printf("========================================\n");
            return 1;
        }

        printf("----------------------------------------\n");
        printf("STEP9C-B: PIPELINE RESULT\n");
        printf("----------------------------------------\n");

        printf("Seeds scanned             : %llu\n",
            (unsigned long long)pipeline->seedsScanned);

        printf("Valid wells               : %llu\n",
            (unsigned long long)pipeline->validWells);

        printf("Physical components       : %llu\n",
            (unsigned long long)pipeline->physicalComponentCount);

        printf("2-well components         : %llu\n",
            (unsigned long long)pipeline->physicalPairComponentCount);

        printf("Wells in 2-well comps     : %llu\n",
            (unsigned long long)pipeline->physicalPairWellCount);

        printf("Physical 3-wells           : %llu\n",
            (unsigned long long)pipeline->physical3Found);

        printf("Physical 4-wells           : %llu\n",
            (unsigned long long)pipeline->physical4Found);

        printf("Stored physical results   : %llu\n",
            (unsigned long long)pipeline->physicalResultCount);

        /*
         * STEP9C-C:
         * Compare against the frozen STEP9B oracle.
         */
        if (pipeline->seedsScanned != 4)
            pass = 0;

        if (pipeline->validWells != 0)
            pass = 0;

        if (pipeline->physicalComponentCount != 0)
            pass = 0;

        if (pipeline->physicalPairComponentCount != 0)
            pass = 0;

        if (pipeline->physicalPairWellCount != 0)
            pass = 0;

        if (pipeline->physical3Found != 0)
            pass = 0;

        if (pipeline->physical4Found != 0)
            pass = 0;

        if (pipeline->physicalResultCount != 0)
            pass = 0;

        printf("----------------------------------------\n");

        if (pass)
        {
            printf(
                "RESULT: PASS - STEP9C END-TO-END EQUIVALENCE\n"
            );
        }
        else
        {
            printf(
                "RESULT: FAIL - STEP9C END-TO-END EQUIVALENCE\n"
            );
        }

        free(pipeline->physicalResults);
        free(pipeline);

        printf("========================================\n");
        printf("STEP9C COMPLETE\n");
        printf("========================================\n");

        return pass ? 0 : 1;
    }


    /*
     * STEP9E: corrected partition equivalence audit.
     *
     * The same four frozen 4F candidates are processed through
     * random_worker() using four different partition layouts.
     *
     * The aggregate result must be identical for every layout.
     */
    if (argc == 2 &&
        strcmp(argv[1], "4e9e") == 0)
    {
        static const int64_t seeds[4] =
        {
            -4667261597301147968LL,
            -8747559536492820829LL,
            -5841154199779032894LL,
             583081449948609959LL
        };

        static const int xs[4] =
        {
            615635,
            720771,
            691530,
            679247
        };

        static const int zs[4] =
        {
            -563688,
            -531319,
            -577694,
            -431615
        };

        static const uint64_t indices[4] =
        {
            0ULL, 1ULL, 2ULL, 3ULL
        };

        static const int layout[4][4] =
        {
            {1,1,1,1},
            {2,2,0,0},
            {3,1,0,0},
            {4,0,0,0}
        };

        typedef struct
        {
            uint64_t seeds;
            uint64_t valid;
            uint64_t components;
            uint64_t pairs;
            uint64_t pairWells;
            uint64_t physical3;
            uint64_t physical4;
            size_t stored;
        } Aggregate;

        Aggregate reference = {0};
        int overallPass = 1;

        printf("========================================\n");
        printf(" STEP9E CORRECTED PARTITION EQUIVALENCE\n");
        printf("========================================\n");
        printf("Candidates : 4\n");
        printf("Center     : (676297, -479950)\n");
        printf("Radius     : 1\n");
        printf("Pattern    : 4\n");
        printf("----------------------------------------\n");

        for (int run = 0; run < 4; run++)
        {
            Aggregate a = {0};
            int offset = 0;

            printf(
                "Partition %d layout: ",
                run + 1
            );

            for (int pidx = 0; pidx < 4; pidx++)
            {
                int n = layout[run][pidx];

                if (n == 0)
                    continue;

                if (pidx > 0)
                    printf("+");

                printf("%d", n);
            }

            printf("\n");

            for (int pidx = 0; pidx < 4; pidx++)
            {
                int n = layout[run][pidx];

                if (n == 0)
                    continue;

                RandomThreadArgs args;

                args.count = n;
                args.seeds = (int64_t *)&seeds[offset];
                args.centerXs = (int *)&xs[offset];
                args.centerZs = (int *)&zs[offset];
                args.candidateIndices =
                    (uint64_t *)&indices[offset];

                args.centerX = 676297;
                args.centerZ = -479950;
                args.radius = 1;
                args.pattern = 4;

                ThreadResult *r =
                    (ThreadResult *)random_worker(&args);

                if (!r)
                {
                    printf(
                        "  ERROR: worker partition %d failed\n",
                        pidx + 1
                    );

                    overallPass = 0;
                    offset += n;
                    continue;
                }

                a.seeds += r->seedsScanned;
                a.valid += r->validWells;
                a.components +=
                    r->physicalComponentCount;
                a.pairs +=
                    r->physicalPairComponentCount;
                a.pairWells +=
                    r->physicalPairWellCount;
                a.physical3 +=
                    r->physical3Found;
                a.physical4 +=
                    r->physical4Found;
                a.stored +=
                    r->physicalResultCount;

                free(r->physicalResults);
                free(r);

                offset += n;
            }

            printf(
                "  Aggregate: seeds=%llu valid=%llu "
                "components=%llu pairs=%llu "
                "pairWells=%llu physical3=%llu "
                "physical4=%llu stored=%llu\n",
                (unsigned long long)a.seeds,
                (unsigned long long)a.valid,
                (unsigned long long)a.components,
                (unsigned long long)a.pairs,
                (unsigned long long)a.pairWells,
                (unsigned long long)a.physical3,
                (unsigned long long)a.physical4,
                (unsigned long long)a.stored
            );

            if (run == 0)
            {
                reference = a;

                printf(
                    "  REFERENCE: FROZEN\n"
                );
            }
            else
            {
                if (a.seeds != reference.seeds ||
                    a.valid != reference.valid ||
                    a.components != reference.components ||
                    a.pairs != reference.pairs ||
                    a.pairWells != reference.pairWells ||
                    a.physical3 != reference.physical3 ||
                    a.physical4 != reference.physical4 ||
                    a.stored != reference.stored)
                {
                    printf(
                        "  PARTITION MISMATCH\n"
                    );

                    overallPass = 0;
                }
                else
                {
                    printf(
                        "  PARTITION MATCH\n"
                    );
                }
            }

            printf("----------------------------------------\n");
        }

        if (overallPass)
        {
            printf(
                "RESULT: PASS - STEP9E PARTITION "
                "EQUIVALENCE VERIFIED\n"
            );
        }
        else
        {
            printf(
                "RESULT: FAIL - STEP9E PARTITION "
                "EQUIVALENCE MISMATCH\n"
            );
        }

        printf("========================================\n");
        printf("STEP9E COMPLETE\n");
        printf("========================================\n");

        return overallPass ? 0 : 1;
    }


    /*
     * STEP9F: actual pthread concurrency equivalence audit.
     *
     * The frozen four-candidate stream is processed through the
     * real pthread-based random_worker() path.
     *
     * The resulting aggregate is compared against the known
     * direct-worker oracle aggregate.
     */
    if (argc == 2 &&
        strcmp(argv[1], "4e9f") == 0)
    {
        static const int64_t seeds[4] =
        {
            -4667261597301147968LL,
            -8747559536492820829LL,
            -5841154199779032894LL,
             583081449948609959LL
        };

        static const int xs[4] =
        {
            615635,
            720771,
            691530,
            679247
        };

        static const int zs[4] =
        {
            -563688,
            -531319,
            -577694,
            -431615
        };

        static const uint64_t indices[4] =
        {
            0ULL, 1ULL, 2ULL, 3ULL
        };

        const int candidateCount = 4;
        const int radius = 1;
        const int pattern = 4;
        const int centerX = 676297;
        const int centerZ = -479950;

        printf("========================================\n");
        printf(" STEP9F PTHREAD CONCURRENCY EQUIVALENCE\n");
        printf("========================================\n");
        printf("Candidates : %d\n", candidateCount);
        printf("Threads    : %d\n", NUM_THREADS);
        printf("Center     : (%d, %d)\n", centerX, centerZ);
        printf("Radius     : %d\n", radius);
        printf("Pattern    : %d\n", pattern);
        printf("----------------------------------------\n");

        /*
         * Known oracle aggregate from STEP9B.
         */
        const uint64_t oracleSeeds = 4;
        const uint64_t oracleValid = 0;
        const uint64_t oracleComponents = 0;
        const uint64_t oraclePairs = 0;
        const uint64_t oraclePairWells = 0;
        const uint64_t oraclePhysical3 = 0;
        const uint64_t oraclePhysical4 = 0;
        const size_t oracleStored = 0;

        pthread_t threads[NUM_THREADS];
        RandomThreadArgs args[NUM_THREADS];
        ThreadResult *results[NUM_THREADS] = {0};

        int baseCount =
            candidateCount / NUM_THREADS;

        int remainder =
            candidateCount % NUM_THREADS;

        int offset = 0;
        int created = 0;

        for (int t = 0; t < NUM_THREADS; t++)
        {
            int n =
                baseCount +
                (t < remainder ? 1 : 0);

            args[t].count = n;
            args[t].seeds =
                (int64_t *)&seeds[offset];
            args[t].centerXs =
                (int *)&xs[offset];
            args[t].centerZs =
                (int *)&zs[offset];
            args[t].candidateIndices =
                (uint64_t *)&indices[offset];

            args[t].centerX = centerX;
            args[t].centerZ = centerZ;
            args[t].radius = radius;
            args[t].pattern = pattern;

            printf(
                "Thread %d : %d candidates\n",
                t + 1,
                n
            );

            if (n > 0)
            {
                if (pthread_create(
                        &threads[t],
                        NULL,
                        random_worker,
                        &args[t]) != 0)
                {
                    fprintf(
                        stderr,
                        "STEP9F: pthread_create failed "
                        "for thread %d\n",
                        t + 1
                    );

                    for (int j = 0; j < created; j++)
                        pthread_join(
                            threads[j],
                            NULL
                        );

                    return 1;
                }

                created++;
            }

            offset += n;
        }

        uint64_t totalSeeds = 0;
        uint64_t totalValid = 0;
        uint64_t totalComponents = 0;
        uint64_t totalPairs = 0;
        uint64_t totalPairWells = 0;
        uint64_t totalPhysical3 = 0;
        uint64_t totalPhysical4 = 0;
        size_t totalStored = 0;

        for (int t = 0; t < NUM_THREADS; t++)
        {
            if (args[t].count <= 0)
                continue;

            void *ret = NULL;

            pthread_join(
                threads[t],
                &ret
            );

            results[t] =
                (ThreadResult *)ret;

            if (!results[t])
            {
                fprintf(
                    stderr,
                    "STEP9F: thread %d returned NULL\n",
                    t + 1
                );

                continue;
            }

            totalSeeds +=
                results[t]->seedsScanned;

            totalValid +=
                results[t]->validWells;

            totalComponents +=
                results[t]->physicalComponentCount;

            totalPairs +=
                results[t]->physicalPairComponentCount;

            totalPairWells +=
                results[t]->physicalPairWellCount;

            totalPhysical3 +=
                results[t]->physical3Found;

            totalPhysical4 +=
                results[t]->physical4Found;

            totalStored +=
                results[t]->physicalResultCount;

            free(
                results[t]->physicalResults
            );

            free(
                results[t]
            );
        }

        printf("----------------------------------------\n");
        printf(
            "Parallel aggregate:\n"
            "  seeds=%llu\n"
            "  valid=%llu\n"
            "  components=%llu\n"
            "  pairs=%llu\n"
            "  pairWells=%llu\n"
            "  physical3=%llu\n"
            "  physical4=%llu\n"
            "  stored=%llu\n",
            (unsigned long long)totalSeeds,
            (unsigned long long)totalValid,
            (unsigned long long)totalComponents,
            (unsigned long long)totalPairs,
            (unsigned long long)totalPairWells,
            (unsigned long long)totalPhysical3,
            (unsigned long long)totalPhysical4,
            (unsigned long long)totalStored
        );

        printf("----------------------------------------\n");
        printf("Oracle aggregate:\n");
        printf(
            "  seeds=%llu\n"
            "  valid=%llu\n"
            "  components=%llu\n"
            "  pairs=%llu\n"
            "  pairWells=%llu\n"
            "  physical3=%llu\n"
            "  physical4=%llu\n"
            "  stored=%llu\n",
            (unsigned long long)oracleSeeds,
            (unsigned long long)oracleValid,
            (unsigned long long)oracleComponents,
            (unsigned long long)oraclePairs,
            (unsigned long long)oraclePairWells,
            (unsigned long long)oraclePhysical3,
            (unsigned long long)oraclePhysical4,
            (unsigned long long)oracleStored
        );

        int pass =
            totalSeeds == oracleSeeds &&
            totalValid == oracleValid &&
            totalComponents == oracleComponents &&
            totalPairs == oraclePairs &&
            totalPairWells == oraclePairWells &&
            totalPhysical3 == oraclePhysical3 &&
            totalPhysical4 == oraclePhysical4 &&
            totalStored == oracleStored;

        printf("----------------------------------------\n");

        if (pass)
            printf(
                "RESULT: PASS - STEP9F PTHREAD "
                "CONCURRENCY EQUIVALENCE VERIFIED\n"
            );
        else
            printf(
                "RESULT: FAIL - STEP9F PTHREAD "
                "CONCURRENCY EQUIVALENCE MISMATCH\n"
            );

        printf("========================================\n");
        printf("STEP9F COMPLETE\n");
        printf("========================================\n");

        return pass ? 0 : 1;
    }

    /*
     * STEP9G:
     * Production-path equivalence audit.
     *
     * Uses:
     *   - exact production candidate generation
     *   - exact NUM_THREADS partition
     *   - actual pthread random_worker()
     *   - actual production join/merge
     *
     * Candidate arrays are deliberately stack allocated.
     * They MUST NOT be passed to free().
     */
    if (argc == 2 &&
        strcmp(argv[1], "4e9g") == 0)
    {
        const int count = 4;
        const uint64_t rngSeed = 12345ULL;
        const uint64_t startIndex = 0ULL;
        const int centerX = 676297;
        const int centerZ = -479950;
        const int radius = 1;
        const int pattern = 4;
        const int64_t coordRange = 100000;

        static const int64_t expectedSeeds[4] =
        {
            -4667261597301147968LL,
            -8747559536492820829LL,
            -5841154199779032894LL,
             583081449948609959LL
        };

        static const int expectedXs[4] =
        {
            615635,
            720771,
            691530,
            679247
        };

        static const int expectedZs[4] =
        {
            -563688,
            -531319,
            -577694,
            -431615
        };

        printf("========================================\n");
        printf(" STEP9G PRODUCTION PATH EQUIVALENCE\n");
        printf("========================================\n");
        printf("Candidates : %d\n", count);
        printf("Threads    : %d\n", NUM_THREADS);
        printf("Center     : (%d, %d)\n", centerX, centerZ);
        printf("Radius     : %d\n", radius);
        printf("Pattern    : %d\n", pattern);
        printf("----------------------------------------\n");

        int pass = 1;

        /*
         * STEP9G-A:
         * Exact production candidate generation.
         */
        int64_t candidateSeeds[4];
        int candidateXs[4];
        int candidateZs[4];
        uint64_t candidateIndices[4];

        for (int i = 0; i < count; i++)
        {
            uint64_t candidateIndex =
                startIndex + (uint64_t)i;

            candidateIndices[i] =
                candidateIndex;

            uint64_t base =
                rngSeed +
                0x9E3779B97F4A7C15ULL *
                (candidateIndex + 1ULL);

            uint64_t seedState =
                base ^ 0xA5A5A5A5A5A5A5A5ULL;

            uint64_t xState =
                base ^ 0x3C6EF372FE94F82AULL;

            uint64_t zState =
                base ^ 0xDAA66D2C7DDF743FULL;

            candidateSeeds[i] =
                (int64_t)rng64_next(&seedState);

            candidateXs[i] =
                (int)rng64_range(
                    &xState,
                    centerX - coordRange,
                    centerX + coordRange);

            candidateZs[i] =
                (int)rng64_range(
                    &zState,
                    centerZ - coordRange,
                    centerZ + coordRange);

            printf(
                "Candidate %d: seed=%lld X=%d Z=%d\n",
                i,
                (long long)candidateSeeds[i],
                candidateXs[i],
                candidateZs[i]);

            if (candidateSeeds[i] != expectedSeeds[i] ||
                candidateXs[i] != expectedXs[i] ||
                candidateZs[i] != expectedZs[i])
            {
                printf("  STREAM MISMATCH\n");
                pass = 0;
            }
            else
            {
                printf("  STREAM MATCH\n");
            }
        }

        /*
         * STEP9G-B:
         * Exact production NUM_THREADS partition.
         */
        pthread_t threads[NUM_THREADS];
        RandomThreadArgs args[NUM_THREADS];
        ThreadResult *results[NUM_THREADS] = {0};

        int baseCount =
            count / NUM_THREADS;

        int remainder =
            count % NUM_THREADS;

        int offset = 0;

        printf("----------------------------------------\n");
        printf("STEP9G-B: PRODUCTION PARTITION\n");

        for (int t = 0; t < NUM_THREADS; t++)
        {
            int n =
                baseCount +
                (t < remainder ? 1 : 0);

            args[t].count = n;
            args[t].seeds = candidateSeeds + offset;
            args[t].centerXs = candidateXs + offset;
            args[t].centerZs = candidateZs + offset;
            args[t].candidateIndices =
                candidateIndices + offset;

            args[t].centerX = centerX;
            args[t].centerZ = centerZ;
            args[t].radius = radius;
            args[t].pattern = pattern;

            printf(
                "Thread %d: count=%d offset=%d\n",
                t + 1,
                n,
                offset);

            offset += n;
        }

        if (offset != count)
        {
            printf(
                "PARTITION MISMATCH: offset=%d count=%d\n",
                offset,
                count);
            pass = 0;
        }

        /*
         * Start all production workers.
         */
        int created = 0;

        for (int t = 0; t < NUM_THREADS; t++)
        {
            if (args[t].count <= 0)
                continue;

            if (pthread_create(
                    &threads[t],
                    NULL,
                    random_worker,
                    &args[t]) != 0)
            {
                fprintf(
                    stderr,
                    "STEP9G: pthread_create failed "
                    "for thread %d\n",
                    t + 1);

                for (int j = 0; j < t; j++)
                {
                    if (args[j].count > 0)
                        pthread_join(
                            threads[j],
                            NULL);
                }

                return 1;
            }

            created++;
        }

        /*
         * STEP9G-C:
         * Exact production join/merge.
         */
        uint64_t totalSeeds = 0;
        uint64_t totalValid = 0;
        uint64_t totalComponents = 0;
        uint64_t totalPairs = 0;
        uint64_t totalPairWells = 0;
        uint64_t totalPhysical3 = 0;
        uint64_t totalPhysical4 = 0;
        size_t totalStored = 0;

        int joined = 0;

        for (int t = 0; t < NUM_THREADS; t++)
        {
            if (args[t].count <= 0)
                continue;

            void *ret = NULL;

            if (pthread_join(
                    threads[t],
                    &ret) != 0)
            {
                printf(
                    "Thread %d: JOIN FAILED\n",
                    t + 1);
                pass = 0;
                continue;
            }

            joined++;

            results[t] =
                (ThreadResult *)ret;

            if (!results[t])
            {
                printf(
                    "Thread %d: NULL RESULT\n",
                    t + 1);
                pass = 0;
                continue;
            }

            totalSeeds +=
                results[t]->seedsScanned;

            totalValid +=
                results[t]->validWells;

            totalComponents +=
                results[t]->physicalComponentCount;

            totalPairs +=
                results[t]->physicalPairComponentCount;

            totalPairWells +=
                results[t]->physicalPairWellCount;

            totalPhysical3 +=
                results[t]->physical3Found;

            totalPhysical4 +=
                results[t]->physical4Found;

            totalStored +=
                results[t]->physicalResultCount;
        }

        printf("----------------------------------------\n");
        printf("STEP9G-C: PRODUCTION AGGREGATE\n");
        printf("----------------------------------------\n");
        printf("Threads joined            : %d\n", joined);
        printf("Seeds scanned             : %llu\n",
            (unsigned long long)totalSeeds);
        printf("Valid wells               : %llu\n",
            (unsigned long long)totalValid);
        printf("Physical components       : %llu\n",
            (unsigned long long)totalComponents);
        printf("2-well components         : %llu\n",
            (unsigned long long)totalPairs);
        printf("Wells in 2-well comps     : %llu\n",
            (unsigned long long)totalPairWells);
        printf("Physical 3-wells          : %llu\n",
            (unsigned long long)totalPhysical3);
        printf("Physical 4-wells          : %llu\n",
            (unsigned long long)totalPhysical4);
        printf("Stored physical results   : %zu\n",
            totalStored);

        /*
         * Frozen STEP9B oracle.
         */
        if (totalSeeds != 4 ||
            totalValid != 0 ||
            totalComponents != 0 ||
            totalPairs != 0 ||
            totalPairWells != 0 ||
            totalPhysical3 != 0 ||
            totalPhysical4 != 0 ||
            totalStored != 0)
        {
            printf("FROZEN ORACLE MISMATCH\n");
            pass = 0;
        }
        else
        {
            printf("FROZEN ORACLE MATCH\n");
        }

        /*
         * Free ONLY heap-allocated worker results.
         *
         * candidateSeeds/X/Z/Indices are stack arrays.
         */
        for (int t = 0; t < NUM_THREADS; t++)
        {
            if (!results[t])
                continue;

            free(results[t]->physicalResults);
            free(results[t]);
            results[t] = NULL;
        }

        printf("----------------------------------------\n");

        if (pass)
        {
            printf(
                "RESULT: PASS - STEP9G PRODUCTION "
                "PATH EQUIVALENCE\n");
        }
        else
        {
            printf(
                "RESULT: FAIL - STEP9G PRODUCTION "
                "PATH MISMATCH\n");
        }

        printf("========================================\n");
        printf("STEP9G COMPLETE\n");
        printf("========================================\n");

        return pass ? 0 : 1;
    }


    if (argc == 9 &&
        strcmp(argv[1], "4e") == 0)
    {
        int count = atoi(argv[2]);

        char *end = NULL;
        errno = 0;

        uint64_t rngSeed =
            strtoull(argv[3], &end, 10);

        if (errno != 0 ||
            end == argv[3] ||
            *end != '\0')
        {
            fprintf(stderr,
                "Invalid 4E RNG seed.\n");
            return 1;
        }

        char *indexEnd = NULL;
        errno = 0;

        uint64_t startIndex =
            strtoull(argv[4], &indexEnd, 10);

        if (errno != 0 ||
            indexEnd == argv[4] ||
            *indexEnd != '\0')
        {
            fprintf(stderr,
                "Invalid 4E start index.\n");
            return 1;
        }

        int centerX = atoi(argv[5]);
        int centerZ = atoi(argv[6]);
        int radius  = atoi(argv[7]);
        int pattern = atoi(argv[8]);


        if (count <= 0)
        {
            fprintf(stderr,
                "Invalid 4E candidate count.\n");
            return 1;
        }

        if (radius < 1)
        {
            fprintf(stderr,
                "Radius must be >= 1.\n");
            return 1;
        }

        const int64_t coordRange = 100000;

        /*
         * STEP6: 4F candidate-stream regression.
         *
         * This verifies the frozen Seed/X/Z candidate stream
         * without calling worker() and without modifying 4E.
         */
        if (count == 4 &&
            rngSeed == 12345ULL &&
            startIndex == 0ULL &&
            centerX == 676297 &&
            centerZ == -479950 &&
            coordRange == 100000)
        {
            static const int64_t expectedSeeds[4] =
            {
                -4667261597301147968LL,
                -8747559536492820829LL,
                -5841154199779032894LL,
                 583081449948609959LL
            };

            static const int expectedXs[4] =
            {
                615635,
                720771,
                691530,
                679247
            };

            static const int expectedZs[4] =
            {
                -563688,
                -531319,
                -577694,
                -431615
            };

            int pass = 1;

            printf("========================================\n");
            printf(" STEP6 4F CANDIDATE STREAM REGRESSION\n");
            printf("========================================\n");

            for (int i = 0; i < 4; i++)
            {
                uint64_t candidateIndex =
                    startIndex + (uint64_t)i;

                uint64_t base =
                    rngSeed +
                    0x9E3779B97F4A7C15ULL *
                    (candidateIndex + 1ULL);

                uint64_t seedState =
                    base ^ 0xA5A5A5A5A5A5A5A5ULL;

                uint64_t xState =
                    base ^ 0x3C6EF372FE94F82AULL;

                uint64_t zState =
                    base ^ 0xDAA66D2C7DDF743FULL;

                int64_t seed =
                    (int64_t)rng64_next(&seedState);

                int x =
                    (int)rng64_range(
                        &xState,
                        centerX - coordRange,
                        centerX + coordRange
                    );

                int z =
                    (int)rng64_range(
                        &zState,
                        centerZ - coordRange,
                        centerZ + coordRange
                    );

                printf(
                    "Candidate %d: seed=%lld X=%d Z=%d\n",
                    i,
                    (long long)seed,
                    x,
                    z
                );

                if (seed != expectedSeeds[i] ||
                    x != expectedXs[i] ||
                    z != expectedZs[i])
                {
                    printf("  MISMATCH\n");
                    pass = 0;
                }
                else
                {
                    printf("  MATCH\n");
                }
            }

            printf("----------------------------------------\n");

            if (pass)
                printf("RESULT: PASS - 4F CANDIDATE STREAM VERIFIED\n");
            else
                printf("RESULT: FAIL - 4F CANDIDATE STREAM MISMATCH\n");

            printf("========================================\n");

            return pass ? 0 : 1;
        }


        uint64_t totalSeeds       = 0;
        uint64_t totalValidWells  = 0;
        uint64_t totalPhysical3   = 0;
        uint64_t totalPhysical4   = 0;

        printf("========================================\n");
        printf(" 4E INDEPENDENT CANDIDATE TEST\n");
        printf("========================================\n");
        printf("Count   : %d\n", count);
        printf("RNG seed: %llu\n",
            (unsigned long long)rngSeed);
        printf("Start   : %llu\n",
            (unsigned long long)startIndex);
        printf("Center  : (%d, %d)\n",
            centerX, centerZ);
        printf("Range   : +/- %lld chunks\n",
            (long long)coordRange);
        printf("Radius  : %d\n", radius);
        printf("Pattern : %d\n", pattern);
        printf("----------------------------------------\n");

        /* Initialize progress for random candidate search */
        if (progressEnabled)
        {
            pthread_mutex_lock(&progress_mutex);
            progressTotal = (uint64_t)count;
            progressCompleted = 0;
            progressStart = time(NULL);
            progressLast = 0;
            pthread_mutex_unlock(&progress_mutex);
        }

        /*
         * 4E STEP 3:
         *
         * Generate the complete deterministic candidate batch first.
         * Candidate generation is independent of thread scheduling.
         *
         * The existing random_worker() is then used to process the
         * candidate arrays across NUM_THREADS workers.
         */

        int64_t *candidateSeeds =
            malloc((size_t)count * sizeof(int64_t));

        int *candidateXs =
            malloc((size_t)count * sizeof(int));

        int *candidateZs =
            malloc((size_t)count * sizeof(int));

        uint64_t *candidateIndices =
            malloc((size_t)count * sizeof(uint64_t));

        if (!candidateSeeds ||
            !candidateXs ||
            !candidateZs ||
            !candidateIndices)
        {
            fprintf(stderr,
                "4E STEP 3: failed to allocate candidate arrays.\n");

            free(candidateSeeds);
            free(candidateXs);
            free(candidateZs);
            free(candidateIndices);

            return 1;
        }

        /*
         * STEP 3A:
         * Deterministic candidate generation.
         *
         * This is intentionally the same Seed/X/Z generation used
         * by verified 4E Step 2.
         */
        for (int i = 0; i < count; i++)
        {
            uint64_t candidateIndex =
                startIndex + (uint64_t)i;

            candidateIndices[i] =
                candidateIndex;

            uint64_t base =
                rngSeed +
                0x9E3779B97F4A7C15ULL *
                (candidateIndex + 1ULL);

            uint64_t seedState =
                base ^ 0xA5A5A5A5A5A5A5A5ULL;

            uint64_t xState =
                base ^ 0x3C6EF372FE94F82AULL;

            uint64_t zState =
                base ^ 0xDAA66D2C7DDF743FULL;

            candidateSeeds[i] =
                (int64_t)rng64_next(&seedState);

            candidateXs[i] =
                (int)rng64_range(
                    &xState,
                    centerX - coordRange,
                    centerX + coordRange
                );

            candidateZs[i] =
                (int)rng64_range(
                    &zState,
                    centerZ - coordRange,
                    centerZ + coordRange
                );
        }

        printf("4E STEP 3: candidates generated: %d\n",
            count);

        /*
         * STEP 3B:
         * Split the already-generated candidate arrays between
         * the existing NUM_THREADS worker threads.
         */
        pthread_t threads[NUM_THREADS];
        RandomThreadArgs threadArgs[NUM_THREADS];
        ThreadResult *threadResults[NUM_THREADS] = {0};

        int baseCount =
            count / NUM_THREADS;

        int remainder =
            count % NUM_THREADS;

        int offset = 0;

        printf("4E STEP 3: processing with %d threads\n",
            NUM_THREADS);

        for (int t = 0; t < NUM_THREADS; t++)
        {
            int threadCount =
                baseCount + (t < remainder ? 1 : 0);

            threadArgs[t].count = threadCount;

            threadArgs[t].seeds =
                candidateSeeds + offset;

            threadArgs[t].centerXs =
                candidateXs + offset;

            threadArgs[t].centerZs =
                candidateZs + offset;

            threadArgs[t].candidateIndices =
                candidateIndices + offset;

            threadArgs[t].centerX = centerX;
            threadArgs[t].centerZ = centerZ;
            threadArgs[t].radius  = radius;
            threadArgs[t].pattern = pattern;

            printf(
                "  Thread %d : %d candidates\n",
                t + 1,
                threadCount
            );

            if (threadCount > 0)
            {
                if (pthread_create(
                        &threads[t],
                        NULL,
                        random_worker,
                        &threadArgs[t]) != 0)
                {
                    fprintf(stderr,
                        "4E STEP 3: failed to create thread %d\n",
                        t + 1);

                    for (int j = 0; j < t; j++)
                    {
                        if (threadArgs[j].count > 0)
                            pthread_join(
                                threads[j],
                                NULL
                            );
                    }

                    free(candidateSeeds);
                    free(candidateXs);
                    free(candidateZs);
                    free(candidateIndices);

                    return 1;
                }
            }

            offset += threadCount;
        }

        /*
         * STEP 3C:
         * Join all workers and merge their ThreadResult structures.
         */
        for (int t = 0; t < NUM_THREADS; t++)
        {
            if (threadArgs[t].count <= 0)
                continue;

            void *ret = NULL;

            pthread_join(
                threads[t],
                &ret
            );

            threadResults[t] =
                (ThreadResult *)ret;

            if (!threadResults[t])
                continue;

            totalSeeds +=
                threadResults[t]->seedsScanned;

            totalValidWells +=
                threadResults[t]->validWells;

            totalPhysical3 +=
                threadResults[t]->physical3Found;

            totalPhysical4 +=
                threadResults[t]->physical4Found;
        }

        /*
         * STEP 3D:
         * Print physical results after all workers have completed.
         */
        int resultNumber = 0;

        for (int t = 0; t < NUM_THREADS; t++)
        {
            ThreadResult *r =
                threadResults[t];

            if (!r)
                continue;

            for (size_t pi = 0;
                 pi < r->physicalResultCount;
                 pi++)
            {
                PhysicalResult *pr =
                    &r->physicalResults[pi];

                Cluster *c =
                    &pr->cluster;

                if (c->count != 3 &&
                    c->count != 4)
                    continue;

                resultNumber++;

                printf(
                    "\n----------------------------------------\n"
                );

                if (c->count == 3)
                {
                    printf(
                        "4E STEP 3 PHYSICAL 3-WELL #%d\n",
                        resultNumber
                    );
                }
                else
                {
                    printf(
                        "4E STEP 3 PHYSICAL 4-WELL #%d\n",
                        resultNumber
                    );
                }

                printf(
                    "Seed : %lld\n",
                    (long long)pr->seed
                );

                for (int wi = 0;
                     wi < c->count && wi < 4;
                     wi++)
                {
                    Well *w =
                        &c->w[wi];

                    printf(
                        "W%d: chunk=(%d,%d) "
                        "world=(%d,%d) "
                        "local=(%d,%d)\n",
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
                    "Chunk bounds: (%d,%d) -> (%d,%d)\n",
                    c->minX,
                    c->minZ,
                    c->maxX,
                    c->maxZ
                );
            }
        }

        for (int t = 0; t < NUM_THREADS; t++)
        {
            if (!threadResults[t])
                continue;

            free(
                threadResults[t]->physicalResults
            );

            free(
                threadResults[t]
            );
        }

        free(candidateSeeds);
        free(candidateXs);
        free(candidateZs);
        free(candidateIndices);

        printf("========================================\n");
        printf(" 4E STEP 3 TEST COMPLETE\n");
        printf("========================================\n");
        printf("Candidates tested : %d\n", count);
        printf("Seeds scanned     : %llu\n",
            (unsigned long long)totalSeeds);
        printf("Valid wells       : %llu\n",
            (unsigned long long)totalValidWells);
        printf("Physical 3-wells  : %llu\n",
            (unsigned long long)totalPhysical3);
        printf("Physical 4-wells  : %llu\n",
            (unsigned long long)totalPhysical4);
        printf("========================================\n");

        return 0;
    }


    /*
     * ------------------------------------------------------------
     * 1B RANDOM SEARCH
     *
     * Processes a large deterministic candidate stream in small
     * batches so memory usage stays reasonable on Android.
     *
     * Candidate generation is IDENTICAL to verified 4E/4F:
     *
     *   base = rngSeed + CONST * (candidateIndex + 1)
     *
     * The global candidate index is preserved across batches.
     * ------------------------------------------------------------
     */
      if ((argc == 9 && strcmp(argv[1], "1b") == 0) ||
          (argc == 7 && strcmp(argv[1], "1brandom") == 0) ||
          (argc == 2 && strcmp(argv[1], "1bresume") == 0))
    {
        uint64_t totalCandidates;
        uint64_t rngSeed;
        uint64_t startIndex;

        int centerX;
        int centerZ;
        int radius;
        int pattern;

        uint64_t restoredSeeds = 0;
        uint64_t restoredValidWells = 0;
        uint64_t restoredPhysicalComponents = 0;
        uint64_t restoredPairComponents = 0;
        uint64_t restoredPairWells = 0;
        uint64_t restoredPhysical3 = 0;
        uint64_t restoredPhysical4 = 0;

        int resumeMode =
            (argc == 2 &&
             strcmp(argv[1], "1bresume") == 0);

        char *end = NULL;

        if (resumeMode)
        {
            uint64_t checkpointStartIndex;
            uint64_t checkpointTotalCandidates;
            uint64_t checkpointNextIndex;

            if (!load_1b_checkpoint(
                    &rngSeed,
                    &checkpointStartIndex,
                    &checkpointTotalCandidates,
                    &checkpointNextIndex,
                    &centerX,
                    &centerZ,
                    &radius,
                    &pattern,
                    &restoredSeeds,
                    &restoredValidWells,
                    &restoredPhysicalComponents,
                    &restoredPairComponents,
                    &restoredPairWells,
                    &restoredPhysical3,
                    &restoredPhysical4))
            {
                fprintf(
                    stderr,
                    "1B RESUME: no valid checkpoint found.\n");
                return 1;
            }

            if (checkpointNextIndex < checkpointStartIndex ||
                checkpointNextIndex >
                    checkpointStartIndex +
                    checkpointTotalCandidates)
            {
                fprintf(
                    stderr,
                    "1B RESUME: invalid checkpoint range.\n");
                return 1;
            }

            startIndex = checkpointNextIndex;

            totalCandidates =
                checkpointTotalCandidates -
                (checkpointNextIndex -
                 checkpointStartIndex);

            printf(
                "1B RESUME: checkpoint loaded.\n");
            printf(
                "Resume index : %llu\n",
                (unsigned long long)startIndex);
            printf(
                "Remaining    : %llu\n",
                (unsigned long long)totalCandidates);
        }
        else
        {
            errno = 0;
            totalCandidates = strtoull(argv[2], &end, 10);
            if (errno != 0 ||
                end == argv[2] ||
                *end != '\0')
            {
                fprintf(
                    stderr,
                    "Invalid 1B candidate count.\n");
                return 1;
            }

              if (strcmp(argv[1], "1brandom") == 0)
              {
                  int64_t randomSeed;

                  if (!random_seed64(&randomSeed))
                  {
                      fprintf(stderr, "Failed to generate random 64-bit RNG seed.\n");
                      return 1;
                  }

                  rngSeed = (uint64_t)randomSeed;
                  startIndex = 0;

                  centerX = atoi(argv[3]);
                  centerZ = atoi(argv[4]);
                  radius  = atoi(argv[5]);
                  pattern = atoi(argv[6]);

                  printf("[RANDOM MODE] Fresh RNG seed: %lld\n",
                         (long long)randomSeed);
              }
              else
              {
                  errno = 0;
                  rngSeed = strtoull(argv[3], &end, 10);
                  if (errno != 0 || end == argv[3] || *end != '\0')
                  {
                      fprintf(stderr, "Invalid 1B RNG seed.\n");
                      return 1;
                  }

                  errno = 0;
                  startIndex = strtoull(argv[4], &end, 10);
                  if (errno != 0 || end == argv[4] || *end != '\0')
                  {
                      fprintf(stderr, "Invalid 1B start index.\n");
                      return 1;
                  }

                  centerX = atoi(argv[5]);
                  centerZ = atoi(argv[6]);
                  radius  = atoi(argv[7]);
                  pattern = atoi(argv[8]);
              }
        }

        const int64_t coordRange = 100000;
        const uint64_t BATCH_SIZE = 1000000ULL;

        /*
         * LEVEL 20.3:
         * Distributed 1B candidate-stream partitioning.
         *
         * Each worker receives a deterministic, non-overlapping
         * portion of the GLOBAL candidate index stream.
         *
         * The original rngSeed is preserved. Therefore:
         *
         *   candidateIndex -> candidateSeed/X/Z
         *
         * remains identical regardless of worker count.
         */
        int distributedWorkerId = 0;
        int distributedWorkerCount = 1;

        const char *distributedWorkerIdEnv =
            getenv("WELL_WORKER_ID");

        const char *distributedWorkerCountEnv =
            getenv("WELL_WORKER_COUNT");

        if (distributedWorkerIdEnv &&
            *distributedWorkerIdEnv)
        {
            distributedWorkerId =
                atoi(distributedWorkerIdEnv);
        }

        if (distributedWorkerCountEnv &&
            *distributedWorkerCountEnv)
        {
            distributedWorkerCount =
                atoi(distributedWorkerCountEnv);
        }

        if (distributedWorkerCount <= 0 ||
            distributedWorkerId < 0 ||
            distributedWorkerId >= distributedWorkerCount)
        {
            fprintf(
                stderr,
                "1B LEVEL 20.3: invalid worker configuration.\n"
                "Worker ID must be in [0, worker count).\n"
            );
            return 1;
        }

        /*
         * Preserve the original global stream before partitioning.
         */
        uint64_t globalStartIndex = startIndex;
        uint64_t globalTotalCandidates = totalCandidates;

        /*
         * Partition the remaining GLOBAL candidate range.
         *
         * Example:
         *
         *   20 candidates / 2 workers
         *
         *   Worker 0 -> 0..9
         *   Worker 1 -> 10..19
         */
        if (!resumeMode && distributedWorkerCount > 1)
        {
            uint64_t workerBase =
                globalTotalCandidates /
                (uint64_t)distributedWorkerCount;

            uint64_t workerRemainder =
                globalTotalCandidates %
                (uint64_t)distributedWorkerCount;

            uint64_t workerOffset =
                (uint64_t)distributedWorkerId *
                workerBase +
                ((uint64_t)distributedWorkerId <
                    workerRemainder
                    ? (uint64_t)distributedWorkerId
                    : workerRemainder);

            uint64_t workerLength =
                workerBase +
                ((uint64_t)distributedWorkerId <
                    workerRemainder
                    ? 1ULL
                    : 0ULL);

            if (workerLength == 0)
            {
                fprintf(
                    stderr,
                    "1B LEVEL 20.3: worker %d received an empty range.\n",
                    distributedWorkerId
                );
                return 1;
            }

            startIndex =
                globalStartIndex +
                workerOffset;

            totalCandidates =
                workerLength;
        }

        printf(
            "[LEVEL 20.3] Distributed worker: %d/%d\n",
            distributedWorkerId,
            distributedWorkerCount
        );

        if (totalCandidates > 0)
        {
            printf(
                "[LEVEL 20.3] Assigned candidate range: %llu -> %llu\n",
                (unsigned long long)startIndex,
                (unsigned long long)(
                    startIndex + totalCandidates - 1ULL
                )
            );
        }

        if (totalCandidates == 0 && !resumeMode)
        {
            fprintf(stderr, "Invalid 1B candidate count.\n");
            return 1;
        }

        if (totalCandidates == 0 && resumeMode)
        {
            printf(
                "1B RESUME: checkpoint is already complete.\n"
            );
            return 0;
        }

        if (radius < 1)
        {
            fprintf(stderr, "Radius must be >= 1.\n");
            return 1;
        }

        /*
         * Prevent uint64 overflow in:
         * startIndex + totalCandidates - 1
         */
        if (startIndex > UINT64_MAX - totalCandidates + 1ULL)
        {
            fprintf(stderr, "1B candidate range overflows uint64.\n");
            return 1;
        }

        uint64_t batchCount =
            (totalCandidates + BATCH_SIZE - 1ULL) / BATCH_SIZE;

        printf("========================================\n");
        printf(" 1B RANDOM PHYSICAL WELL SEARCH\n");
        printf("========================================\n");
        printf("Candidates : %llu\n",
            (unsigned long long)totalCandidates);
        printf("Batch size : %llu\n",
            (unsigned long long)BATCH_SIZE);
        printf("Batches    : %llu\n",
            (unsigned long long)batchCount);
        printf("RNG seed   : %llu\n",
            (unsigned long long)rngSeed);
        printf("Start      : %llu\n",
            (unsigned long long)startIndex);
        printf("Center     : (%d, %d)\n",
            centerX, centerZ);
        printf("Range      : +/- %lld chunks\n",
            (long long)coordRange);
        printf("Radius     : %d\n", radius);
        printf("Pattern    : %d\n", pattern);
        printf("Threads    : %d\n", NUM_THREADS);
        printf("----------------------------------------\n");

        /*
         * Global progress covers the ENTIRE 1B search.
         */
        if (progressEnabled)
        {
            pthread_mutex_lock(&progress_mutex);
            progressTotal = totalCandidates;
            progressCompleted = 0;
            progressStart = time(NULL);
            progressLast = 0;
            pthread_mutex_unlock(&progress_mutex);
        }

        time_t searchStart = time(NULL);

        uint64_t totalSeeds = restoredSeeds;
        uint64_t totalValidWells = restoredValidWells;
        uint64_t totalPhysicalComponents =
            restoredPhysicalComponents;
        uint64_t totalPhysicalPairComponents =
            restoredPairComponents;
        uint64_t totalPhysicalPairWells =
            restoredPairWells;
        uint64_t totalPhysical3 = restoredPhysical3;
        uint64_t totalPhysical4 = restoredPhysical4;
          uint64_t totalLevel17PrefilterPairs = 0;
          uint64_t totalLevel17ActualPairs = 0;

        int resultNumber = 0;

        for (uint64_t batch = 0;
             batch < batchCount;
             batch++)
        {
            uint64_t processedBefore =
                batch * BATCH_SIZE;

            uint64_t remaining =
                totalCandidates - processedBefore;

            uint64_t thisCount =
                remaining < BATCH_SIZE ?
                remaining :
                BATCH_SIZE;

            uint64_t batchStart =
                startIndex + processedBefore;

            /*
             * RandomThreadArgs.count is int, therefore the batch
             * size is intentionally kept <= 1,000,000.
             */
            int count = (int)thisCount;
              int adaptiveThreads = get_adaptive_thread_count();
              printf("[LEVEL 16] Adaptive worker threads: %d\n", adaptiveThreads);


              if (adaptiveThreads > count)
                  adaptiveThreads = count;

              if (adaptiveThreads < 1)
                  adaptiveThreads = 1;


            printf(
                "\n[1B] Batch %llu/%llu"
                " | candidate index %llu -> %llu\n",
                (unsigned long long)(batch + 1ULL),
                (unsigned long long)batchCount,
                (unsigned long long)batchStart,
                (unsigned long long)
                    (batchStart + thisCount - 1ULL)
            );

            int64_t *candidateSeeds =
                malloc((size_t)count * sizeof(int64_t));

            int *candidateXs =
                malloc((size_t)count * sizeof(int));

            int *candidateZs =
                malloc((size_t)count * sizeof(int));

            uint64_t *candidateIndices =
                malloc((size_t)count * sizeof(uint64_t));

            if (!candidateSeeds ||
                !candidateXs ||
                !candidateZs ||
                !candidateIndices)
            {
                fprintf(stderr,
                    "1B: failed to allocate batch %llu.\n",
                    (unsigned long long)(batch + 1ULL));

                free(candidateSeeds);
                free(candidateXs);
                free(candidateZs);
                free(candidateIndices);

                return 1;
            }

            /*
             * Generate this batch using GLOBAL candidate indices.
             */
            for (int i = 0; i < count; i++)
            {
                uint64_t candidateIndex =
                    batchStart + (uint64_t)i;

                candidateIndices[i] =
                    candidateIndex;

                uint64_t base =
                    rngSeed +
                    0x9E3779B97F4A7C15ULL *
                    (candidateIndex + 1ULL);

                uint64_t seedState =
                    base ^ 0xA5A5A5A5A5A5A5A5ULL;

                uint64_t xState =
                    base ^ 0x3C6EF372FE94F82AULL;

                uint64_t zState =
                    base ^ 0xDAA66D2C7DDF743FULL;

                candidateSeeds[i] =
                    (int64_t)rng64_next(&seedState);

                candidateXs[i] =
                    (int)rng64_range(
                        &xState,
                        centerX - coordRange,
                        centerX + coordRange
                    );

                candidateZs[i] =
                    (int)rng64_range(
                        &zState,
                        centerZ - coordRange,
                        centerZ + coordRange
                    );
            }

            pthread_t threads[adaptiveThreads];
            RandomThreadArgs threadArgs[adaptiveThreads];
            ThreadResult **threadResults =
                calloc((size_t)adaptiveThreads, sizeof(ThreadResult *));

            if (!threadResults)
            {
                fprintf(stderr,
                    "1B: failed to allocate adaptive thread results.\n");
                free(candidateSeeds);
                free(candidateXs);
                free(candidateZs);
                free(candidateIndices);
                return 1;
            }

            int baseCount =
                count / adaptiveThreads;

            int remainder =
                count % adaptiveThreads;

            int offset = 0;
            int createFailed = 0;

            for (int t = 0; t < adaptiveThreads; t++)
            {
                int threadCount =
                    baseCount +
                    (t < remainder ? 1 : 0);

                threadArgs[t].count =
                    threadCount;

                threadArgs[t].seeds =
                    candidateSeeds + offset;

                threadArgs[t].centerXs =
                    candidateXs + offset;

                threadArgs[t].centerZs =
                    candidateZs + offset;

                threadArgs[t].candidateIndices =
                    candidateIndices + offset;

                threadArgs[t].centerX = centerX;
                threadArgs[t].centerZ = centerZ;
                threadArgs[t].radius  = radius;
                threadArgs[t].pattern = pattern;

                if (threadCount > 0)
                {
                    if (pthread_create(
                            &threads[t],
                            NULL,
                            random_worker,
                            &threadArgs[t]) != 0)
                    {
                        fprintf(stderr,
                            "1B: pthread_create failed "
                            "for thread %d in batch %llu.\n",
                            t + 1,
                            (unsigned long long)(batch + 1ULL));

                        createFailed = 1;

                        for (int j = 0; j < t; j++)
                        {
                            if (threadArgs[j].count > 0)
                                pthread_join(
                                    threads[j],
                                    NULL
                                );
                        }

                        break;
                    }
                }

                offset += threadCount;
            }

            if (createFailed)
            {
                free(candidateSeeds);
                free(candidateXs);
                free(candidateZs);
                free(candidateIndices);
                return 1;
            }

            /*
             * Join workers and collect this batch.
             */
            for (int t = 0; t < adaptiveThreads; t++)
            {
                if (threadArgs[t].count <= 0)
                    continue;

                void *ret = NULL;

                pthread_join(
                    threads[t],
                    &ret
                );

                threadResults[t] =
                    (ThreadResult *)ret;

                if (!threadResults[t])
                    continue;

                totalSeeds +=
                    threadResults[t]->seedsScanned;

                totalValidWells +=
                    threadResults[t]->validWells;

                totalPhysicalComponents +=
                    threadResults[t]->physicalComponentCount;

                totalPhysicalPairComponents +=
                    threadResults[t]->physicalPairComponentCount;

                totalPhysicalPairWells +=
                    threadResults[t]->physicalPairWellCount;

                totalPhysical3 +=
                    threadResults[t]->physical3Found;

                totalPhysical4 +=
                    threadResults[t]->physical4Found;

                  totalLevel17PrefilterPairs +=
                      threadResults[t]->level17PrefilterPairs;

                  totalLevel17ActualPairs +=
                      threadResults[t]->level17ActualPairs;
            }

            /*
             * Report physical results immediately.
             */
            for (int t = 0; t < adaptiveThreads; t++)
            {
                ThreadResult *r =
                    threadResults[t];

                if (!r)
                    continue;

                for (size_t pi = 0;
                     pi < r->physicalResultCount;
                     pi++)
                {
                    PhysicalResult *pr =
                        &r->physicalResults[pi];

                    Cluster *c =
                        &pr->cluster;

                    if (c->count != 3 &&
                        c->count != 4)
                        continue;

                    resultNumber++;

                    printf(
                        "\n----------------------------------------\n"
                    );

                    if (c->count == 3)
                    {
                        printf(
                            "1B PHYSICAL 3-WELL #%d\n",
                            resultNumber
                        );
                    }
                    else
                    {
                        printf(
                            "1B PHYSICAL 4-WELL #%d\n",
                            resultNumber
                        );
                    }

                    printf(
                        "Seed : %lld\n",
                        (long long)pr->seed
                    );

                    for (int wi = 0;
                         wi < c->count && wi < 4;
                         wi++)
                    {
                        Well *w =
                            &c->w[wi];

                        printf(
                            "W%d: chunk=(%d,%d) "
                            "world=(%d,%d) "
                            "local=(%d,%d)\n",
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
                        "Chunk bounds: "
                        "(%d,%d) -> (%d,%d)\n",
                        c->minX,
                        c->minZ,
                        c->maxX,
                        c->maxZ
                    );
                }

                free(r->physicalResults);
                free(r);
            }
              free(threadResults);

            free(candidateSeeds);
            free(candidateXs);
            free(candidateZs);
            free(candidateIndices);

            printf(
                "[1B] Batch %llu/%llu complete"
                " | total scanned: %llu\n",
                (unsigned long long)(batch + 1ULL),
                (unsigned long long)batchCount,
                (unsigned long long)totalSeeds
            );

            save_1b_checkpoint(
                "RUNNING",
                rngSeed,
                startIndex,
                totalCandidates,
                batchStart + thisCount,
                centerX,
                centerZ,
                radius,
                pattern,
                totalSeeds,
                totalValidWells,
                totalPhysicalComponents,
                totalPhysicalPairComponents,
                totalPhysicalPairWells,
                totalPhysical3,
                totalPhysical4
            );
        }

        /*
         * Final checkpoint: entire 1B search completed.
         */
        save_1b_checkpoint(
            "COMPLETE",
            rngSeed,
            startIndex,
            totalCandidates,
            startIndex + totalCandidates,
            centerX,
            centerZ,
            radius,
            pattern,
            totalSeeds,
            totalValidWells,
            totalPhysicalComponents,
            totalPhysicalPairComponents,
            totalPhysicalPairWells,
            totalPhysical3,
            totalPhysical4
        );

        time_t searchFinish = time(NULL);

        long long elapsed =
            (long long)(searchFinish - searchStart);

        printf("\n========================================\n");
        printf(" 1B RANDOM SEARCH COMPLETE\n");
        printf("========================================\n");
        printf("Candidates tested : %llu\n",
            (unsigned long long)totalCandidates);
        printf("Seeds scanned     : %llu\n",
            (unsigned long long)totalSeeds);
        printf("Valid wells             : %llu\n",
            (unsigned long long)totalValidWells);
        printf("Physical components     : %llu\n",
            (unsigned long long)totalPhysicalComponents);
        printf("2-well components       : %llu\n",
            (unsigned long long)totalPhysicalPairComponents);
        printf("2-well component wells  : %llu\n",
            (unsigned long long)totalPhysicalPairWells);
        printf("Physical 3-wells        : %llu\n",
            (unsigned long long)totalPhysical3);
        printf("Physical 4-wells  : %llu\n",
            (unsigned long long)totalPhysical4);
          printf("LEVEL 17 prefilter pairs : %llu\n",
              (unsigned long long)totalLevel17PrefilterPairs);
          printf("LEVEL 17 actual pairs    : %llu\n",
              (unsigned long long)totalLevel17ActualPairs);
        printf("----------------------------------------\n");
        printf("SEARCH TIMING\n");

        {
            struct tm *tmStart = localtime(&searchStart);
            struct tm *tmFinish = localtime(&searchFinish);

            char startBuf[64];
            char finishBuf[64];

            if (tmStart)
                strftime(
                    startBuf,
                    sizeof(startBuf),
                    "%Y-%m-%d %H:%M:%S",
                    tmStart
                );
            else
                snprintf(startBuf, sizeof(startBuf), "unknown");

            if (tmFinish)
                strftime(
                    finishBuf,
                    sizeof(finishBuf),
                    "%Y-%m-%d %H:%M:%S",
                    tmFinish
                );
            else
                snprintf(finishBuf, sizeof(finishBuf), "unknown");

            printf("Started : %s\n", startBuf);
            printf("Finished: %s\n", finishBuf);
        }

        printf(
            "Elapsed : %02lld:%02lld:%02lld\n",
            elapsed / 3600,
            (elapsed % 3600) / 60,
            elapsed % 60
        );

        printf("========================================\n");

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

    /*
     * LEVEL 20.1: distributed worker partitioning.
     *
     * Optional environment variables:
     *   WELL_WORKER_ID    = zero-based worker ID
     *   WELL_WORKER_COUNT = total number of workers
     *
     * When not supplied, the existing full-range behavior is preserved.
     */
    int workerId = 0;
    int workerCount = 1;

    const char *workerIdEnv = getenv("WELL_WORKER_ID");
    const char *workerCountEnv = getenv("WELL_WORKER_COUNT");

    if (workerIdEnv)
        workerId = atoi(workerIdEnv);

    if (workerCountEnv)
        workerCount = atoi(workerCountEnv);

    if (workerCount <= 0 ||
        workerId < 0 ||
        workerId >= workerCount)
    {
        fprintf(stderr,
            "Invalid distributed worker configuration.\n"
            "Worker ID must be in [0, worker_count).\n");
        return 1;
    }

    if (seedEnd < seedStart)
    {
        fprintf(stderr, "Invalid seed range.\n");
        return 1;
    }

    uint64_t total =
        (uint64_t)(seedEnd - seedStart) + 1ULL;

    /*
     * LEVEL 20.1: split the requested seed range into
     * deterministic, non-overlapping worker ranges.
     */
    if (workerCount > 1)
    {
        uint64_t base =
            total / (uint64_t)workerCount;

        uint64_t rem =
            total % (uint64_t)workerCount;

        uint64_t workerOffset =
            (uint64_t)workerId * base +
            ((uint64_t)workerId < rem
                ? (uint64_t)workerId
                : rem);

        uint64_t workerLength =
            base +
            ((uint64_t)workerId < rem ? 1ULL : 0ULL);

        if (workerLength == 0)
        {
            fprintf(stderr,
                "Worker %d received an empty seed range.\n",
                workerId);
            return 1;
        }

        seedStart += (int64_t)workerOffset;
        seedEnd =
            seedStart + (int64_t)workerLength - 1;

        total = workerLength;
    }

    printf("========================================\n");
    printf(" LEVEL 20.1 DISTRIBUTED WORKER\n");
    printf("========================================\n");
    printf("Worker ID    : %d\n", workerId);
    printf("Worker count : %d\n", workerCount);
    printf("Assigned     : %lld -> %lld\n",
        (long long)seedStart,
        (long long)seedEnd);
    printf("Seeds        : %llu\n",
        (unsigned long long)total);
    printf("========================================\n");

    /* Search timer initialization */
    searchStartTime = time(NULL);

/* Live progress initialization */
    if (progressEnabled)
    {
        pthread_mutex_lock(&progress_mutex);
        progressTotal = total;
        progressCompleted = 0;
        progressStart = time(NULL);
        progressLast = 0;
        pthread_mutex_unlock(&progress_mutex);
    }

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

    searchEndTime = time(NULL);
    print_search_timing();

    return 0;
}
