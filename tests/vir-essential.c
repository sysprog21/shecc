volatile unsigned int visible;
unsigned int reduce_data[37];
unsigned long long reduce_wide_data[20];

/* The block after the loop stores the sum before defining any value, so the
 * combined sum must precede that store.
 */
unsigned int reduce_sink;

/* Helpers are defined after main, so they keep their calls and graphs; the
 * leaves above main are the ones inlining replaces.
 */
unsigned int jump32(unsigned int seed);
unsigned int reference32(unsigned int seed);
unsigned long long jump64(unsigned long long seed);
unsigned long long reference64(unsigned long long seed);
unsigned int triangle(unsigned int n);
unsigned int rotate(unsigned int n, unsigned int a, unsigned int b);
unsigned int small_fib(unsigned int n);
unsigned int effectful(unsigned int n);
unsigned char narrow(unsigned int n);
int signed_jump(int seed);
int signed_reference(int seed);
int narrow_tail(signed char n, int total);
unsigned int reduce_even(void);
unsigned int reduce_odd(void);
void reduce_store(void);
unsigned long long reduce_wide(void);
int signed_divide(int x);
int signed_remainder(int x);
unsigned int unsigned_divide(unsigned int x);
unsigned int unsigned_remainder(unsigned int x);
long long wide_divide(long long x);
long long wide_remainder(long long x);
static unsigned int mix_leaf(unsigned int x)
{
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    return x ^ (x >> 16);
}
static unsigned char narrow_leaf(unsigned char x, signed char y)
{
    return x * 3 + y;
}
static short short_leaf(short x)
{
    return x * 1000;
}
int main(void)
{
    for (unsigned int seed = 0; seed < 32; seed++) {
        unsigned int input = 4294967295u - seed * 7919u;
        if (jump32(input) != reference32(input))
            return 1;
        unsigned long long wide = 18446744073709551615ULL - seed * 7919u;
        if (jump64(wide) != reference64(wide))
            return 2;
        if (triangle(seed) != seed * (seed + 1) / 2)
            return 3;
    }
    if (rotate(10, 1, 1) != 89 || small_fib(13) != 233 || narrow(7) != 43)
        return 4;
    visible = 0;
    if (effectful(20) != 210 || visible != 21)
        return 5;
    if (mix_leaf(1) != 1617762472u)
        return 6;
    for (int i = -1025; i <= 1025; i++) {
        int q = signed_divide(i), r = signed_remainder(i);
        if (q * 16 + r != i || r <= -16 || r >= 16 || (i < 0 ? r > 0 : r < 0))
            return 7;
        unsigned int u = (unsigned int) i;
        if (unsigned_divide(u) * 128u + unsigned_remainder(u) != u ||
            unsigned_remainder(u) >= 128u)
            return 8;
    }
    for (int seed = -40; seed < 40; seed += 7)
        if (signed_jump(seed) != signed_reference(seed))
            return 10;
    for (int x = 0; x < 300; x += 37)
        for (int y = -130; y < 130; y += 51)
            if (narrow_leaf(x, y) !=
                (unsigned char) ((unsigned char) x * 3 + (signed char) y))
                return 11;
    if (short_leaf(40) != (short) 40000 || short_leaf(-32) != -32000)
        return 12;
    if (narrow_tail(100, 0) != 5050 || narrow_tail(-1, 7) != 7 ||
        narrow_tail(300, 0) != 990)
        return 13;
    for (unsigned int i = 0; i < 37; i++)
        reduce_data[i] = i * 7 + 3 + (i == 36 ? 4294967000u : 0);
    for (unsigned int i = 0; i < 20; i++)
        reduce_wide_data[i] = 0x100000000ULL * i + 4294967295ULL;
    /* 7 * 35 * 36 / 2 + 3 * 36 + 5; the last element wraps the odd sum. */
    if (reduce_even() != 4523u || reduce_odd() != 4523u + 4294967255u ||
        reduce_wide() != 0x100000000ULL * 190 + 4294967295ULL * 20)
        return 14;
    reduce_store();
    if (reduce_sink != 4523u)
        return 15;
    if (signed_divide(-2147483647 - 1) != -134217728 ||
        signed_remainder(-2147483647 - 1) != 0 ||
        wide_divide(-9223372036854775807LL - 1) != -8388608 ||
        wide_remainder(-9223372036854775807LL - 1) != 0 ||
        wide_divide(-1099511627777LL) != -1 ||
        wide_remainder(-1099511627777LL) != -1)
        return 9;
    return 0;
}
unsigned int jump32(unsigned int seed)
{
    for (unsigned int i = 2; i < 37; i += 3)
        seed = seed * 1664525u + i * 7u + 1013904223u;
    return seed;
}
unsigned int reference32(unsigned int seed)
{
    for (unsigned int i = 2; i < 37; i += 3) {
        visible++;
        seed = seed * 1664525u + i * 7u + 1013904223u;
    }
    return seed;
}
unsigned long long jump64(unsigned long long seed)
{
    for (unsigned long long i = 3; i < 102; i += 5)
        seed =
            seed * 6364136223846793005ULL + i * 23ULL + 1442695040888963407ULL;
    return seed;
}
unsigned long long reference64(unsigned long long seed)
{
    for (unsigned long long i = 3; i < 102; i += 5) {
        visible++;
        seed =
            seed * 6364136223846793005ULL + i * 23ULL + 1442695040888963407ULL;
    }
    return seed;
}
unsigned int triangle(unsigned int n)
{
    if (!n)
        return 0;
    return triangle(n - 1) + n;
}
unsigned int rotate(unsigned int n, unsigned int a, unsigned int b)
{
    if (!n)
        return a;
    return rotate(n - 1, b, a + b);
}
unsigned int small_fib(unsigned int n)
{
    if (n < 2)
        return n;
    return small_fib(n - 1) + small_fib(n - 2);
}
unsigned int effectful(unsigned int n)
{
    visible++;
    if (!n)
        return 0;
    return effectful(n - 1) + n;
}
unsigned char narrow(unsigned int n)
{
    if (!n)
        return 250;
    return narrow(n - 1) + 7;
}
int signed_jump(int seed)
{
    for (int i = 1; i < 40; i += 4)
        seed = seed * 3 - i;
    return seed;
}
int signed_reference(int seed)
{
    for (int i = 1; i < 40; i += 4) {
        visible++;
        seed = seed * 3 - i;
    }
    return seed;
}
int narrow_tail(signed char n, int total)
{
    if (n <= 0)
        return total;
    return narrow_tail(n - 1, total + n);
}
unsigned int reduce_even(void)
{
    unsigned int s = 5, *p = reduce_data;
    for (unsigned int i = 0; i < 36; i++)
        s += *p++;
    return s;
}
unsigned int reduce_odd(void)
{
    unsigned int s = 5, *p = reduce_data;
    for (unsigned int i = 0; i < 37; i++)
        s += *p++;
    return s;
}
void reduce_store(void)
{
    unsigned int s = 5, *p = reduce_data;
    for (unsigned int i = 0; i < 36; i++)
        s += *p++;
    reduce_sink = s;
}
unsigned long long reduce_wide(void)
{
    unsigned long long s = 0, *p = reduce_wide_data;
    for (unsigned int i = 0; i < 20; i++)
        s += *p++;
    return s;
}
int signed_divide(int x)
{
    return x / 16;
}
int signed_remainder(int x)
{
    return x % 16;
}
unsigned int unsigned_divide(unsigned int x)
{
    return x / 128u;
}
unsigned int unsigned_remainder(unsigned int x)
{
    return x % 128u;
}
long long wide_divide(long long x)
{
    return x / 1099511627776LL;
}
long long wide_remainder(long long x)
{
    return x % 1099511627776LL;
}
