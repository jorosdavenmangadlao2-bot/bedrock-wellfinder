#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int x, z;
} Well;

static int wells_physically_touch(const Well *a, const Well *b)
{
    int amaxX = a->x + 4;
    int amaxZ = a->z + 4;
    int bmaxX = b->x + 4;
    int bmaxZ = b->z + 4;

    int xgap = 0;
    int zgap = 0;

    if (amaxX < b->x)
        xgap = b->x - amaxX - 1;
    else if (bmaxX < a->x)
        xgap = a->x - bmaxX - 1;

    if (amaxZ < b->z)
        zgap = b->z - amaxZ - 1;
    else if (bmaxZ < a->z)
        zgap = a->z - bmaxZ - 1;

    return xgap == 0 && zgap == 0;
}

int main(void)
{
    /*
     * Synthetic physical 4-well chain.
     *
     * W1 = X 0..4
     * W2 = X 5..9
     * W3 = X 10..14
     * W4 = X 15..19
     *
     * All four form ONE connected component.
     */

    Well wells[4] = {
        {0, 0},
        {5, 0},
        {10, 0},
        {15, 0}
    };

    int visited[4] = {0};
    int queue[4];

    int qhead = 0;
    int qtail = 0;
    int count = 0;

    queue[qtail++] = 0;
    visited[0] = 1;

    while (qhead < qtail)
    {
        int cur = queue[qhead++];
        count++;

        for (int j = 0; j < 4; j++)
        {
            if (visited[j])
                continue;

            if (wells_physically_touch(
                    &wells[cur],
                    &wells[j]))
            {
                visited[j] = 1;
                queue[qtail++] = j;
            }
        }
    }

    printf("========================================\n");
    printf(" Step 6 Physical Component Test\n");
    printf("========================================\n");

    for (int i = 0; i < 4; i++)
    {
        printf(
            "W%d: X=%d..%d Z=%d..%d\n",
            i + 1,
            wells[i].x,
            wells[i].x + 4,
            wells[i].z,
            wells[i].z + 4);
    }

    printf("----------------------------------------\n");
    printf("BFS component count: %d wells\n", count);
    printf("----------------------------------------\n");

    if (count == 4)
        printf("RESULT: PASS - ONE CONNECTED 4-WELL COMPONENT\n");
    else
        printf("RESULT: FAIL\n");

    printf("========================================\n");

    return count == 4 ? 0 : 1;
}
