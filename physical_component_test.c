#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int x;
    int z;
} Well;

static int footprints_touch(const Well *a, const Well *b)
{
    int dx = abs(a->x - b->x);
    int dz = abs(a->z - b->z);

    /*
     * Two 5x5 footprints are physically connected when:
     *
     *   horizontal: dx <= 5 and dz <= 4
     *   vertical:   dx <= 4 and dz <= 5
     *
     * Corner-only contact (5,5) is NOT connected.
     */
    return (dx <= 5 && dz <= 4) ||
           (dx <= 4 && dz <= 5);
}

static int connected_component(Well *w, int n)
{
    int visited[16] = {0};
    int queue[16];
    int head = 0;
    int tail = 0;

    visited[0] = 1;
    queue[tail++] = 0;

    while (head < tail)
    {
        int cur = queue[head++];

        for (int i = 0; i < n; i++)
        {
            if (visited[i])
                continue;

            if (footprints_touch(&w[cur], &w[i]))
            {
                visited[i] = 1;
                queue[tail++] = i;
            }
        }
    }

    for (int i = 0; i < n; i++)
    {
        if (!visited[i])
            return 0;
    }

    return 1;
}

int main(void)
{
    /*
     * Artificial physical-connectivity regression test.
     *
     * Every well has a 5x5 footprint:
     *
     * W1 W2
     * W3 W4
     *
     * Horizontal/vertical neighbors touch exactly at their edges.
     * Diagonal wells do NOT touch.
     */

    Well w[4] = {
        {0, 0},
        {5, 0},
        {0, 5},
        {5, 5}
    };

    printf("========================================\n");
    printf(" Physical Component Regression Test\n");
    printf("========================================\n");

    for (int i = 0; i < 4; i++)
    {
        printf("W%d footprint: X=%d..%d Z=%d..%d\n",
            i + 1,
            w[i].x,
            w[i].x + 4,
            w[i].z,
            w[i].z + 4);
    }

    printf("----------------------------------------\n");

    int edges = 0;

    for (int i = 0; i < 4; i++)
    {
        for (int j = i + 1; j < 4; j++)
        {
            int touch = footprints_touch(&w[i], &w[j]);

            printf(
                "W%d-W%d: %s\n",
                i + 1,
                j + 1,
                touch ? "TOUCH" : "SEPARATE"
            );

            if (touch)
                edges++;
        }
    }

    printf("----------------------------------------\n");
    printf("Physical connections: %d\n", edges);

    int connected = connected_component(w, 4);

    printf("----------------------------------------\n");

    if (connected)
        printf("RESULT: ONE CONNECTED 4-WELL COMPONENT\n");
    else
        printf("RESULT: NOT CONNECTED\n");

    printf("========================================\n");

    return connected ? 0 : 1;
}
