static unsigned long long folded_left = 0x80000000u << 1LL;
static unsigned long long folded_inverted = ~(0x80000000u << 1LL);
static unsigned long long narrow_left(unsigned int value, long long count)
{
    return value << count;
}
static unsigned long long narrow_right(unsigned int value,
                                       unsigned long long count)
{
    return value >> count;
}
static unsigned long long inverted(unsigned int value, long long count)
{
    return ~(value << count);
}
static unsigned long long widened(unsigned int value, long long count)
{
    return (value << count) + 0x100000000ULL;
}
static long long signed_right(int value, unsigned long long count)
{
    return value >> count;
}
static int promoted_left(unsigned short value, long long count)
{
    return value << count;
}
static unsigned long long wide_left(unsigned long long value, long long count)
{
    return value << count;
}
static long long wide_right(long long value, unsigned long long count)
{
    return value >> count;
}
static unsigned long long compound(unsigned int value, long long count)
{
    return value <<= count;
}
int main(void)
{
    return folded_left != 0ULL || folded_inverted != 0xffffffffULL ||
           narrow_left(0x80000000u, 1LL) != 0ULL ||
           narrow_left(0xffffffffu, 4LL) != 0xfffffff0ULL ||
           narrow_right(0x80000000u, 31ULL) != 1ULL ||
           inverted(0x80000000u, 1LL) != 0xffffffffULL ||
           widened(0x80000000u, 1LL) != 0x100000000ULL ||
           signed_right(-2147483647 - 1, 31ULL) != -1LL ||
           promoted_left(65535, 8LL) != 16776960 ||
           wide_left(1ULL, 32LL) != 0x100000000ULL ||
           wide_left(0x8000000000000000ULL, 1LL) != 0ULL ||
           wide_right(-4294967296LL, 32ULL) != -1LL ||
           compound(0x80000000u, 1LL) != 0ULL;
}
