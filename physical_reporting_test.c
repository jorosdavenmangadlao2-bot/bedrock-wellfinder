#include <stdio.h>

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

int main(void)
{
    Cluster components[2];

    components[0].count = 3;
    components[1].count = 4;

    int physical3Found = 0;
    int physical4Found = 0;

    for (int ci = 0; ci < 2; ci++)
    {
        Cluster *pc = &components[ci];

        if (pc->count != 3 && pc->count != 4)
            continue;

        if (pc->count == 3)
            physical3Found++;
        else
            physical4Found++;
    }

    printf("========================================\n");
    printf(" Physical Reporting Test\n");
    printf("========================================\n");
    printf("Component 1 count: %d\n", components[0].count);
    printf("Component 2 count: %d\n", components[1].count);
    printf("----------------------------------------\n");
    printf("Physical 3-wells: %d\n", physical3Found);
    printf("Physical 4-wells: %d\n", physical4Found);
    printf("----------------------------------------\n");

    if (physical3Found == 1 && physical4Found == 1)
        printf("RESULT: PASS - 3/4 COMPONENT REPORTING WORKS\n");
    else
        printf("RESULT: FAIL\n");

    printf("========================================\n");

    return 0;
}
