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
int signed_bits(int x)
{
    return (x ^ 0x40000000) | (-2147483647 - 1);
}
unsigned int shifted_bits(unsigned int x)
{
    return (x ^ 0xa5a5a5a5u) >> 16;
}
int signed_shift(int x)
{
    return x >> 13;
}
unsigned long long wide_bits(unsigned long long x)
{
    return (x ^ 0x8000000100000000ULL) >> 32;
}
unsigned int left_wrap(unsigned int x)
{
    return x << 2;
}
long long left_signed(unsigned int x)
{
    return (long long) (int) (x << 1);
}
unsigned long long left_unsigned(unsigned int x)
{
    return (unsigned long long) (x << 1);
}
unsigned long long left_wide(unsigned long long x)
{
    return x << 2;
}
unsigned long long left_mixed(unsigned int x)
{
    unsigned int narrow = x << 2;
    unsigned long long wide = (unsigned long long) x << 2;
    return wide + narrow;
}
long long scaled_negative(int x)
{
    return (long long) (x * 16);
}
long long shifted_sign_bit(unsigned int x)
{
    return (long long) (int) (x << 31);
}
#define MEMORY_ARITHMETIC(name, type, op) \
    type name(type *p, type x)            \
    {                                     \
        for (int i = 0; i < 4; i++)       \
            x op *p++;                    \
        return x;                         \
    }
MEMORY_ARITHMETIC(memory_sum, unsigned int, +=)
MEMORY_ARITHMETIC(memory_sub, unsigned int, -=)
MEMORY_ARITHMETIC(memory_and, unsigned int, &=)
MEMORY_ARITHMETIC(memory_or, unsigned int, |=)
MEMORY_ARITHMETIC(memory_xor, unsigned int, ^=)
MEMORY_ARITHMETIC(memory_signed, int, +=)
MEMORY_ARITHMETIC(memory_wide, long long, +=)
int memory_flags(unsigned int *p)
{
    unsigned int x = 15;
    int zeros = 0;
    for (int i = 0; i < 4; i++)
        if ((x &= *p++) == 0)
            zeros++;
    return zeros;
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
    if ((long long) signed_bits(0x12345678) != -768321928LL)
        return 13;
    if (shifted_bits(0x80000000u) != 0x25a5u)
        return 14;
    if ((long long) signed_shift(-123456789) != -15071LL)
        return 15;
    if (wide_bits(0x123456789abcdef0ULL) != 0x92345679ULL)
        return 16;
    if (left_wrap(0x80000001u) != 4u)
        return 17;
    if (left_signed(0x40000000) != -2147483648LL)
        return 18;
    if (left_unsigned(0x80000001u) != 2ULL)
        return 19;
    if (left_wide(0x80000001ULL) != 0x200000004ULL)
        return 20;
    if (left_mixed(0x80000001u) != 0x200000008ULL)
        return 21;
    if (scaled_negative(-12345) != -197520LL)
        return 22;
    if (shifted_sign_bit(1) != -2147483648LL)
        return 23;
    unsigned int values[4] = {1, 2, 4, 8};
    int negatives[4] = {-1, -2, -4, -8};
    long long wide_values[4] = {-0x100000001LL, 2, 4, 8};
    if (memory_sum(values, 0xfffffff0u) != 0xffffffffu ||
        memory_sub(values, 0) != 0xfffffff1u || memory_and(values, 15) != 0 ||
        memory_or(values, 0) != 15 || memory_xor(values, 15) != 0 ||
        memory_flags(values) != 3 ||
        (long long) memory_signed(negatives, 0) != -15LL ||
        memory_wide(wide_values, 0) != -0xfffffff3LL)
        return 24;
    return 0;
}
