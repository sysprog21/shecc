#include <stdarg.h>
int object = 19;
long long add(long long a, long long b)
{
    return a + b;
}
long long live(long long a, long long b)
{
    return add(a, a) + b;
}
long long pair(int n, long long a, long long b)
{
    return a + b + n;
}
long long split(int a, int b, int c, int d, int e, int f, int g, long long w)
{
    return w + a + b + c + d + e + f + g;
}
signed char narrow(signed char x)
{
    return x;
}
int *choose(int *p, int n)
{
    if (n)
        return p;
    return 0;
}
long long unused(int ignored, long long value)
{
    return value;
}
long long variadic(int ignored, long long last, ...)
{
    va_list ap;
    va_start(ap, last);
    long long x = va_arg(ap, long long);
    int y = va_arg(ap, int);
    va_end(ap);
    return last + x + y;
}
long long pinned(long long value, int n)
{
    long long x = add(n, n);
    if (n)
        return value + x;
    return value - x;
}
int unused_pair(long long ignored, int value)
{
    return value;
}
int main(void)
{
    if (live(7, 13) != 27)
        return 1;
    if (pair(-3, 0x12345678ffffffffLL, 5) != 0x1234567900000001LL)
        return 2;
    if (split(1, 2, 3, 4, 5, 6, 7, 0x12345678ffffffffLL) !=
        0x123456790000001bLL)
        return 3;
    if (narrow(-127) != -127)
        return 4;
    if (choose(&object, 1) != &object || choose(&object, 0) != 0)
        return 5;
    if (unused(99, 0x12345678ffffffffLL) != 0x12345678ffffffffLL)
        return 6;
    if (variadic(99, 7LL, 0x12345678ffffffffLL, 3) != 0x1234567900000009LL)
        return 7;
    if (pinned(0x12345678ffffffffLL, 3) != 0x1234567900000005LL)
        return 8;
    if (pinned(9, 0) != 9 || unused_pair(0x12345678ffffffffLL, 17) != 17)
        return 9;
    return 0;
}
