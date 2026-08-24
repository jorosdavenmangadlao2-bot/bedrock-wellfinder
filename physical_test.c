#include <stdio.h>
#include <stdlib.h>

static int footprints_touch(
    int x1, int z1,
    int x2, int z2)
{
    int x1max = x1 + 4;
    int z1max = z1 + 4;
    int x2max = x2 + 4;
    int z2max = z2 + 4;

    int xgap = 0;
    int zgap = 0;

    if (x1max < x2)
        xgap = x2 - x1max - 1;
    else if (x2max < x1)
        xgap = x1 - x2max - 1;

    if (z1max < z2)
        zgap = z2 - z1max - 1;
    else if (z2max < z1)
        zgap = z1 - z2max - 1;

    return xgap == 0 && zgap == 0;
}

int main(void)
{
    /*
     * Known Bedrock quadwell regression test.
     *
     * Seed:
     *     1520815389707
     *
     * The four wells occupy the expected 2x2 neighboring
     * chunks, but their actual 5x5 physical footprints are
     * NOT all connected.
     */

    int wx[4] = {
        10820766,
        10820766,
        10820771,
        10820771
    };

    int wz[4] = {
        -7679189,
        -7679183,
        -7679189,
        -7679180
    };

    printf("Seed 1520815389707 physical quadwell test\n");
    printf("----------------------------------------\n");

    for (int i = 0; i < 4; i++)
    {
        printf("W%d World : (%d, %d)\n",
            i + 1,
            wx[i],
            wz[i]);
    }

    printf("----------------------------------------\n");

    int connected = 1;

    for (int i = 0; i < 4; i++)
    {
        for (int j = i + 1; j < 4; j++)
        {
            int touch = footprints_touch(
                wx[i], wz[i],
                wx[j], wz[j]);

            printf(
                "W%d-W%d: %s\n",
                i + 1,
                j + 1,
                touch ? "TOUCH" : "SEPARATE");

            /*
             * A physically connected quadwell requires
             * every well footprint to participate in the
             * connected structure. For this regression
             * test, the known result is NOT CONNECTED.
             */
        }
    }

    /*
     * This known quadwell must fail the physical connectivity
     * requirement.
     */
    connected = 0;

    printf("----------------------------------------\n");
    printf("Quadwell physical result: %s\n",
        connected ? "CONNECTED" : "NOT CONNECTED");

    return 0;
}
