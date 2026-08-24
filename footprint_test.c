#include <stdio.h>
#include <stdlib.h>

static int wells_physically_touch(int ax, int az, int bx, int bz)
{
    int dx = abs(ax - bx);
    int dz = abs(az - bz);

    return (dx <= 5 && dz <= 4) ||
           (dx <= 4 && dz <= 5);
}

int main(void)
{
    printf("5x5 footprint test\n");

    printf("dx=5 dz=4 : %s\n",
        wells_physically_touch(0,0,5,4) ? "TOUCH" : "SEPARATE");

    printf("dx=5 dz=5 : %s\n",
        wells_physically_touch(0,0,5,5) ? "TOUCH" : "SEPARATE");

    printf("dx=4 dz=4 : %s\n",
        wells_physically_touch(0,0,4,4) ? "TOUCH" : "SEPARATE");

    return 0;
}
