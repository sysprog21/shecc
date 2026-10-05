unsigned int uproduct(unsigned int x, unsigned int y)
{
    return x * y;
}
int product(int x, int y)
{
    return x * y;
}
long long wide(long long x, long long y)
{
    return x * y + x - y;
}
long long subtract_min(long long x)
{
    return x - (-2147483648LL) + x;
}
int subtract_min_int(int x)
{
    return x - (-2147483647 - 1) + x;
}
int add(int x, int y)
{
    return x + y;
}
int sub(int x, int y)
{
    return x - y;
}
int main(void)
{
    if ((long long) product(-12345, 7) != -86415LL)
        return 1;
    if ((unsigned long long) uproduct(4294967295u, 2u) != 4294967294ULL)
        return 2;
    if (wide(4294967296LL, 3LL) != 17179869181LL)
        return 3;
    if ((long long) add(-2147483647, -1) != -2147483648LL)
        return 4;
    if ((long long) sub(-12345, 321) != -12666LL)
        return 5;
    int a[3];
    int *p = a + 2;
    if (p - a != 2)
        return 6;
    if (product(12345, 31) != 382695)
        return 7;
    if (product(-12345, 16) != -197520)
        return 8;
    if (subtract_min(10LL) != 2147483668LL)
        return 9;
    if ((long long) product(-12345, 1) != -12345LL)
        return 10;
    if ((unsigned long long) uproduct(4294967295u, 1u) != 4294967295ULL)
        return 11;
    if (subtract_min_int(10) != (int) 2147483668u)
        return 12;
    return 0;
}
