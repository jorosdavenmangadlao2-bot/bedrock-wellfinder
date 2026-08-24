#include <stdio.h>
#include <stdint.h>
#include "finders.h"

typedef struct {
    int cx;
    int cz;
    int expectedX;
    int expectedZ;
} TestCase;

int main(void)
{
    uint64_t seed = 1520815389707ULL;

    TestCase tests[] = {
        {676297, -479950, 14, 11},
        {676298, -479950,  3, 11},
        {676297, -479949, 14,  1},
        {676298, -479949,  3,  4}
    };

    int total = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;

    printf("========================================\n");
    printf(" Quadwell Regression Test\n");
    printf("========================================\n");
    printf("Seed: %llu\n\n", (unsigned long long)seed);

    for (int i = 0; i < total; i++)
    {
        Pos p;

        int found = getStructurePos(
            Desert_Well,
            MC_1_13,
            seed,
            tests[i].cx,
            tests[i].cz,
            &p
        );

        int actualX = p.x & 15;
        int actualZ = p.z & 15;

        int match =
            found &&
            actualX == tests[i].expectedX &&
            actualZ == tests[i].expectedZ;

        printf("Case %d\n", i + 1);
        printf("Chunk    : (%d, %d)\n",
               tests[i].cx, tests[i].cz);
        printf("Expected : (%d, %d)\n",
               tests[i].expectedX, tests[i].expectedZ);

        if (found)
            printf("Actual   : (%d, %d)\n", actualX, actualZ);
        else
            printf("Actual   : NO WELL\n");

        printf("Result   : %s\n\n",
               match ? "PASS" : "FAIL");

        if (match)
            passed++;
    }

    printf("========================================\n");
    printf("Passed: %d / %d\n", passed, total);
    printf("========================================\n");

    return passed == total ? 0 : 1;
}
