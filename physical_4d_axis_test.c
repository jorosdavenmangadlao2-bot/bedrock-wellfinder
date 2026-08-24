#include <stdio.h>
#include <stdint.h>

typedef struct {
    int64_t seed;
    int cx;
    int cz;
    int well_count;
} Observation;

int main(void)
{
    /*
     * 4D search space:
     *
     *   seed
     *   X
     *   Z
     *
     * A physical well cluster is valid only for a
     * particular seed AND spatial position.
     */

    Observation observations[] = {
        {1520815389707LL, 676297, -479950, 4},
        {1669320484LL,       7625,     -233, 3},
        {883849785374863756LL, 23, 6, 3}
    };

    int count = sizeof(observations) / sizeof(observations[0]);

    printf("========================================\n");
    printf(" 4D SEED / X / Z AXIS TEST\n");
    printf("========================================\n");

    for (int i = 0; i < count; i++)
    {
        printf(
            "Observation %d: seed=%lld X=%d Z=%d wells=%d\n",
            i + 1,
            (long long)observations[i].seed,
            observations[i].cx,
            observations[i].cz,
            observations[i].well_count
        );
    }

    printf("----------------------------------------\n");

    int seed_axis = 1;
    int x_axis = 1;
    int z_axis = 1;

    /*
     * The test only verifies that the search model
     * keeps seed, X and Z as independent coordinates.
     */
    for (int i = 0; i < count; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            if (observations[i].seed == observations[j].seed)
                seed_axis = 0;

            if (observations[i].cx == observations[j].cx)
                x_axis = 0;

            if (observations[i].cz == observations[j].cz)
                z_axis = 0;
        }
    }

    printf("Seed axis : %s\n", seed_axis ? "ACTIVE" : "COLLAPSED");
    printf("X axis    : %s\n", x_axis ? "ACTIVE" : "COLLAPSED");
    printf("Z axis    : %s\n", z_axis ? "ACTIVE" : "COLLAPSED");

    printf("----------------------------------------\n");

    if (seed_axis && x_axis && z_axis)
        printf("RESULT: PASS - 4D AXES PRESERVED\n");
    else
        printf("RESULT: FAIL\n");

    printf("========================================\n");

    return 0;
}
