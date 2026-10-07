static int unequal(int a, int b)
{
    return a != b;
}
static int shared(int a, int b)
{
    int e = a == b;
    return !e + 2 * e;
}
static int wide(unsigned long long a, unsigned long long b)
{
    return a != b;
}
static int pointer(int *a, int *b)
{
    return !(a == b);
}
static int nonzero(int a, int b)
{
    return (a == b) == 2;
}
static int bit(int a)
{
    if (a & 8)
        return 7;
    return 9;
}
int main(void)
{
    int a = 1, b = 2;
    if (unequal(-3, -3) || unequal(-3, 3) != 1)
        return 1;
    if (shared(-3, -3) != 2 || shared(-3, 3) != 1)
        return 2;
    if (wide(0x100000000ULL, 0) != 1 || wide(0x100000000ULL, 0x100000000ULL))
        return 3;
    if (pointer(&a, &a) || pointer(&a, &b) != 1)
        return 4;
    if (nonzero(3, 3) || nonzero(3, 4))
        return 5;
    if (bit(8) != 7 || bit(0) != 9 || bit(-1) != 7)
        return 6;
    return 0;
}
