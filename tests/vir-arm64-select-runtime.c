unsigned int choose(unsigned int c, unsigned int x, unsigned int y)
{
    unsigned int result;
    if (c < 9)
        result = (x ^ y) + x;
    else
        result = (x + y) - y;
    return result;
}
unsigned long long choose_wide(unsigned long long c,
                               unsigned long long x,
                               unsigned long long y)
{
    unsigned long long result;
    if (c == 7)
        result = (x & y) ^ x;
    else
        result = (x | y) + y;
    return result;
}
unsigned int narrow_masks(unsigned int x)
{
    return ((x & 255U) ^ 65535U) | 7U;
}
unsigned long long wide_masks(unsigned long long x)
{
    return ((x & 0xffffffffffULL) ^ 0x7fffffffffffffffULL) | 31ULL;
}
volatile unsigned int observed;
static unsigned int bump(unsigned int x)
{
    observed++;
    return x + 9;
}
static unsigned int effect_arms(unsigned int c, unsigned int x)
{
    unsigned int a, b;
    if (c) {
        a = bump(x);
        b = observed;
    } else {
        observed = x;
        a = observed;
        b = x + 1;
    }
    return a + b;
}
static unsigned int live_condition(unsigned int c, unsigned int x)
{
    int cond = c < 9;
    unsigned int value;
    if (cond)
        value = x + 1;
    else
        value = x ^ 17;
    return value + cond;
}
static long long signed_masks(long long x)
{
    return (x & 0x7fffffffLL) ^ 0xffffLL;
}
/* LLONG_MIN has no immediate form, so its magnitude must not wrap into one. */
static long long add_min(long long x)
{
    return x + (-9223372036854775807LL - 1);
}
static long long sub_min(long long x)
{
    return x - (-9223372036854775807LL - 1);
}
/* A 64-bit product added to another value becomes one MADD. */
static unsigned long long multiply_add(unsigned long long a,
                                       unsigned long long b,
                                       unsigned long long c)
{
    return a * b + c;
}

int main(void)
{
    for (unsigned int i = 0; i < 64; i++) {
        unsigned int x = 0x80000000U + i * 1664525U;
        unsigned int y = 0xffffffffU - i * 1013904223U;
        unsigned int expected = i < 9 ? (x ^ y) + x : (x + y) - y;
        if (choose(i, x, y) != expected)
            return 1;
        unsigned long long a = 0x8000000100000000ULL + x;
        unsigned long long b = 0xfffffffe00000000ULL + y;
        unsigned long long expected_wide = i == 7 ? (a & b) ^ a : (a | b) + b;
        if (choose_wide(i, a, b) != expected_wide)
            return 2;
        if (narrow_masks(x) != (((x & 255U) ^ 65535U) | 7U))
            return 3;
        if (wide_masks(a) !=
            (((a & 0xffffffffffULL) ^ 0x7fffffffffffffffULL) | 31ULL))
            return 4;
    }
    observed = 0;
    if (effect_arms(1, 7) != 17 || observed != 1)
        return 5;
    if (effect_arms(0, 7) != 15 || observed != 7)
        return 6;
    if (live_condition(1, 30) != 32 || live_condition(20, 30) != 15)
        return 7;
    if (signed_masks(-1LL) != (0x7fffffffLL ^ 0xffffLL))
        return 8;
    if (signed_masks(-9223372036854775807LL) != (1LL ^ 0xffffLL))
        return 9;
    if (add_min(1) != -9223372036854775807LL)
        return 10;
    if (sub_min(2) != -9223372036854775806LL)
        return 11;
    if (multiply_add(0x100000001ULL, 0xfffffffffULL, 0x8000000000000000ULL) !=
        0x8000000effffffffULL)
        return 12;
    return 0;
}
