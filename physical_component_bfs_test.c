#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int cx, cz;
    int x, z;
} Well;

static int wells_physically_touch(const Well *a, const Well *b)
{
    int dx = abs(a->x - b->x);
    int dz = abs(a->z - b->z);

    return (dx <= 5 && dz <= 4) ||
           (dx <= 4 && dz <= 5);
}

static int scan_component(
    Well *grid,
    int width,
    int height)
{
    int total = width * height;

    int *visited = calloc((size_t)total, sizeof(int));
    int *queue = malloc((size_t)total * sizeof(int));

    if (!visited || !queue)
    {
        free(visited);
        free(queue);
        return 0;
    }

    int component_count = 0;

    for (int i = 0; i < total; i++)
    {
        if (visited[i] || grid[i].cx < 0)
            continue;

        int head = 0;
        int tail = 0;

        queue[tail++] = i;
        visited[i] = 1;

        int count = 0;

        while (head < tail)
        {
            int cur = queue[head++];
            count++;

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
                            &grid[cur],
                            &grid[ni]))
                    {
                        visited[ni] = 1;
                        queue[tail++] = ni;
                    }
                }
            }
        }

        printf("Component %d: %d wells\n",
               component_count + 1,
               count);

        component_count++;
    }

    free(visited);
    free(queue);

    return component_count;
}

int main(void)
{
    /*
     * 3x3 chunk grid.
     *
     * These four wells form:
     *
     *   W1 W2
     *   W3 W4
     *
     * Their 5x5 footprints are physically connected.
     */

    int width = 3;
    int height = 3;

    Well grid[9];

    for (int i = 0; i < 9; i++)
    {
        grid[i].cx = -1;
        grid[i].cz = -1;
        grid[i].x = 0;
        grid[i].z = 0;
    }

    /*
     * W1
     */
    grid[0].cx = 0;
    grid[0].cz = 0;
    grid[0].x = 0;
    grid[0].z = 0;

    /*
     * W2
     */
    grid[1].cx = 1;
    grid[1].cz = 0;
    grid[1].x = 5;
    grid[1].z = 0;

    /*
     * W3
     */
    grid[3].cx = 0;
    grid[3].cz = 1;
    grid[3].x = 0;
    grid[3].z = 5;

    /*
     * W4
     */
    grid[4].cx = 1;
    grid[4].cz = 1;
    grid[4].x = 5;
    grid[4].z = 5;

    printf("========================================\n");
    printf(" Physical Component BFS Regression Test\n");
    printf("========================================\n");

    int components =
        scan_component(grid, width, height);

    printf("----------------------------------------\n");
    printf("Components found: %d\n", components);

    if (components == 1)
        printf("RESULT: PASS - ONE PHYSICAL COMPONENT\n");
    else
        printf("RESULT: FAIL\n");

    printf("========================================\n");

    return components == 1 ? 0 : 1;
}
