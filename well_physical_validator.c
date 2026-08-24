#include <stdio.h>
#include <math.h>

typedef struct
{
    int x;
    int z;
} Well;

/*
 * Known wells from the verified quadwell seed:
 *
 * (10820766,-7679189)
 * (10820766,-7679183)
 * (10820760,-7679141)
 * (10820771,-7679189)
 * (10820771,-7679180)
 *
 * These are BLOCK coordinates.
 * No chunk-neighbor logic is used here.
 */

static double distance(const Well *a, const Well *b)
{
    double dx = (double)a->x - (double)b->x;
    double dz = (double)a->z - (double)b->z;

    return sqrt(dx * dx + dz * dz);
}

static void print_pair(const char *name, const Well *a, const Well *b)
{
    printf(
        "%s: %.3f blocks\n",
        name,
        distance(a, b)
    );
}

int main(void)
{
    Well wells[] =
    {
        {10820766, -7679189},
        {10820766, -7679183},
        {10820760, -7679141},
        {10820771, -7679189},
        {10820771, -7679180}
    };

    const int count = sizeof(wells) / sizeof(wells[0]);

    printf("========================================\n");
    printf(" PHYSICAL WELL DISTANCE VALIDATOR\n");
    printf("========================================\n");
    printf("Input wells : %d\n", count);
    printf("Coordinate system : BLOCK X/Z\n");
    printf("Chunk-neighbor logic : DISABLED\n");
    printf("----------------------------------------\n");

    for (int i = 0; i < count; i++)
    {
        printf(
            "WELL #%d : (%d,%d)\n",
            i + 1,
            wells[i].x,
            wells[i].z
        );
    }

    printf("----------------------------------------\n");
    printf("PAIRWISE DISTANCES\n");
    printf("----------------------------------------\n");

    for (int i = 0; i < count; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            char name[32];

            snprintf(
                name,
                sizeof(name),
                "Well #%d <-> Well #%d",
                i + 1,
                j + 1
            );

            print_pair(name, &wells[i], &wells[j]);
        }
    }

    /*
     * For this first validation we use a deliberately small
     * physical threshold of 32 blocks.
     *
     * This is NOT yet the final definition of a physical cluster.
     * It only tests whether the known wells are physically close.
     */
    const double LIMIT = 32.0;

    printf("----------------------------------------\n");
    printf("PHYSICAL GROUP TEST\n");
    printf("----------------------------------------\n");
    printf("Test radius : %.1f blocks\n", LIMIT);

    int found3 = 0;
    int found4 = 0;

    /*
     * 3-well combinations
     */
    for (int a = 0; a < count; a++)
    {
        for (int b = a + 1; b < count; b++)
        {
            for (int c = b + 1; c < count; c++)
            {
                double ab = distance(&wells[a], &wells[b]);
                double ac = distance(&wells[a], &wells[c]);
                double bc = distance(&wells[b], &wells[c]);

                if (ab <= LIMIT &&
                    ac <= LIMIT &&
                    bc <= LIMIT)
                {
                    printf(
                        "3-WELL GROUP:\n"
                        "  #%d (%d,%d)\n"
                        "  #%d (%d,%d)\n"
                        "  #%d (%d,%d)\n"
                        "  distances: %.3f, %.3f, %.3f\n\n",
                        a + 1, wells[a].x, wells[a].z,
                        b + 1, wells[b].x, wells[b].z,
                        c + 1, wells[c].x, wells[c].z,
                        ab, ac, bc
                    );

                    found3 = 1;
                }
            }
        }
    }

    /*
     * 4-well combinations
     */
    for (int a = 0; a < count; a++)
    {
        for (int b = a + 1; b < count; b++)
        {
            for (int c = b + 1; c < count; c++)
            {
                for (int d = c + 1; d < count; d++)
                {
                    double ab = distance(&wells[a], &wells[b]);
                    double ac = distance(&wells[a], &wells[c]);
                    double ad = distance(&wells[a], &wells[d]);

                    double bc = distance(&wells[b], &wells[c]);
                    double bd = distance(&wells[b], &wells[d]);
                    double cd = distance(&wells[c], &wells[d]);

                    if (ab <= LIMIT &&
                        ac <= LIMIT &&
                        ad <= LIMIT &&
                        bc <= LIMIT &&
                        bd <= LIMIT &&
                        cd <= LIMIT)
                    {
                        printf(
                            "4-WELL GROUP:\n"
                            "  #%d (%d,%d)\n"
                            "  #%d (%d,%d)\n"
                            "  #%d (%d,%d)\n"
                            "  #%d (%d,%d)\n"
                            "  distances:\n"
                            "    %.3f %.3f %.3f\n"
                            "    %.3f %.3f %.3f\n\n",
                            a + 1, wells[a].x, wells[a].z,
                            b + 1, wells[b].x, wells[b].z,
                            c + 1, wells[c].x, wells[c].z,
                            d + 1, wells[d].x, wells[d].z,
                            ab, ac, ad,
                            bc, bd, cd
                        );

                        found4 = 1;
                    }
                }
            }
        }
    }

    printf("========================================\n");
    printf("VALIDATION RESULT\n");
    printf("========================================\n");
    printf("Physical 3-well : %s\n",
        found3 ? "FOUND" : "NOT FOUND");
    printf("Physical 4-well : %s\n",
        found4 ? "FOUND" : "NOT FOUND");
    printf("========================================\n");

    return 0;
}
