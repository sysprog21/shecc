volatile int guard;
long long test_add(long long a, long long b)
{
    return a + b;
}
long long
test_mix(int x0, int x1, int x2, int x3, int x4, int x5, int x6, long long y)
{
    return x0 + x1 + x2 + x3 + x4 + x5 + x6 + y;
}
long long vir_mixed(void)
{
    if (guard)
        return test_mix(guard, guard, guard, guard, guard, guard, guard, guard);
    return 0;
}
long long test_va(int last, ...)
{
    return last;
}
long long vir_vararg(void)
{
    if (guard)
        return test_va(guard, guard);
    return 0;
}
int vir_loop(void)
{
    if (guard)
        return 1;
    return 0;
}
long long vir_wide(void)
{
    if (guard)
        return test_add(guard, guard);
    return 0;
}
long long vir_signed(void)
{
    if (guard)
        return 1;
    return 0;
}
int vir_sr(void)
{
    if (guard)
        return 1;
    return 0;
}
long long test_va_aggregate(int *last, ...)
{
    if (guard)
        return *last;
    return 0;
}
long long vir_mul_left(long long a, long long b)
{
    if (guard)
        return a + b;
    return 0;
}
long long vir_mul_right(long long a, long long b)
{
    if (guard)
        return a + b;
    return 0;
}
long long vir_pointer_add(void *base, long long offset)
{
    if (guard)
        return (long long) base + offset;
    return 0;
}
int main(void)
{
    if ((unsigned long long) vir_mul_left(0x12345678ffffffffULL,
                                          0xfedcba9812345678ULL) !=
            0x314c741fedcba988ULL ||
        (unsigned long long) vir_mul_right(0x12345678ffffffffULL,
                                           0xfedcba9812345678ULL) !=
            0x314c741fedcba988ULL ||
        (unsigned long long) vir_mul_left(-0x100000001LL, 0x200000003LL) !=
            0xfffffffafffffffdULL ||
        (unsigned long long) vir_mul_right(-0x100000001LL, 0x200000003LL) !=
            0xfffffffafffffffdULL)
        return 8;
    if (sizeof(void *) == 8 &&
        (vir_pointer_add((void *) 5, 0x100000000LL) != 0x100000005LL ||
         (unsigned long long) vir_pointer_add((void *) 5, -0x100000001LL) !=
             0xffffffff00000004ULL))
        return 9;
    if (test_va_aggregate(&guard, 0x123456789abcdef0LL) != 0x123456789abcdef0LL)
        return 7;
    if (vir_sr() != 28)
        return 6;
    if (vir_loop() != 97)
        return 1;
    if (vir_wide() != 0x369d036afffffffdLL)
        return 2;
    if (vir_signed() != -1LL)
        return 3;
    if (vir_mixed() != 0x123456789abcdee3LL)
        return 4;
    if (vir_vararg() != 0x123456789abcdef0LL)
        return 5;
    return 0;
}
