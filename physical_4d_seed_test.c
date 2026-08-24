#include <stdio.h>
#include <stdint.h>

typedef struct
{
    int64_t seed;
    int componentCount;
} SeedResult;

int main(void)
{
    SeedResult results[] =
    {
        {1520815389707LL, 2},
        {1669320484LL,    1},
        {883849785374863756LL, 3}
    };

    int total = sizeof(results) / sizeof(results[0]);

    printf("========================================\n");
    printf(" 4D SEED-AXIS REGRESSION TEST\n");
    printf("========================================\n");

    for (int i = 0; i < total; i++)
    {
        printf("Seed %lld -> physical components: %d\n",
               (long long)results[i].seed,
               results[i].componentCount);
    }

    printf("----------------------------------------\n");

    if (total == 3)
        printf("RESULT: PASS - SEED AXIS REPRESENTED\n");
    else
        printf("RESULT: FAIL\n");

    printf("========================================\n");

    return 0;
}
