#include <stdio.h>

static int footprints_touch(int x1, int z1, int x2, int z2)
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
    int wx[3] = {
        0, 5, 10
    };

    int wz[3] = {
        0, 0, 0
    };

    printf("========================================\n");
    printf(" Physical 3-Well Regression Test\n");
    printf("========================================\n");

    for (int i = 0; i < 3; i++)
    {
        printf("W%d footprint: X=%d..%d Z=%d..%d\n",
            i + 1,
            wx[i],
            wx[i] + 4,
            wz[i],
            wz[i] + 4);
    }

    printf("----------------------------------------\n");

    int edges = 0;

    for (int i = 0; i < 3; i++)
    {
        for (int j = i + 1; j < 3; j++)
        {
            int touch = footprints_touch(
                wx[i], wz[i],
                wx[j], wz[j]);

            printf("W%d-W%d: %s\n",
                i + 1,
                j + 1,
                touch ? "TOUCH" : "SEPARATE");

            if (touch)
                edges++;
        }
    }

    printf("----------------------------------------\n");
    printf("Physical connections: %d\n", edges);
    printf("----------------------------------------\n");

    /*
     * Expected:
     *
     * W1-W2 = TOUCH
     * W2-W3 = TOUCH
     * W1-W3 = SEPARATE
     *
     * Therefore all 3 wells belong to
     * one connected component.
     */

    int connected[3] = {1, 0, 0};
    int changed = 1;

    while (changed)
    {
        changed = 0;

        for (int i = 0; i < 3; i++)
        {
            if (!connected[i])
                continue;

            for (int j = 0; j < 3; j++)
            {
                if (connected[j])
                    continue;

                if (footprints_touch(
                        wx[i], wz[i],
                        wx[j], wz[j]))
                {
                    connected[j] = 1;
                    changed = 1;
                }
            }
        }
    }

    int allConnected = 1;

    for (int i = 0; i < 3; i++)
    {
        if (!connected[i])
            allConnected = 0;
    }

    printf("RESULT: %s\n",
        allConnected
            ? "PASS - ONE CONNECTED 3-WELL COMPONENT"
            : "FAIL");

    printf("========================================\n");

    return allConnected ? 0 : 1;
}
