#include <stdio.h>

int main()
{
    printf("F(10) = %d\n", 2147483647);
    printf("F(10) = %d\n", -2147483648);
    printf("F(10) = %d\n", -2147483647);
    printf("F(10) = %d\n", -214748364);
    printf("F(10) = %11d\n", -214748364);
    printf("F(10) = %16d\n", -214748364);
    return 0;
}

