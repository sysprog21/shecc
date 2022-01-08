#include <stdio.h>

int main()
{
    printf("%d * %d = %d\n", 0, 0, 0 * 0);
    printf("%d * %d = %d\n", 0, 1, 0 * 1);
    printf("%d * %d = %d\n", 1, 0, 1 * 0);
    printf("%d * %d = %d\n", 1, 1, 1 * 1);
    printf("%d * %d = %d\n", 1, 9, 1 * 9);
    printf("%d * %d = %d\n", 9, 1, 9 * 1);
    printf("%d * %d = %d\n", 2, 7, 2 * 7);
    printf("%d * %d = %d\n", 7, 2, 7 * 2);
    printf("%d * %d = %d\n", 13, 17, 13 * 17);
    printf("%d * %d = %d\n", 17, 13, 17 * 13);

    return 0;
}
