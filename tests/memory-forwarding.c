/*
 * Values the optimizer carries from a store or an earlier load to a later load,
 * stores it removes, and arithmetic it rewrites. Every case reads its operands
 * through volatile objects, so the inputs stay unknown to the compiler while
 * the stores, loads and aliases under test are plain. The program returns the
 * number of cases that gave the wrong answer.
 */
struct pair {
    int a, b, c, d, e;
};

int g, h;
int table[8];
int fails;
volatile int vzero;
volatile int vone = 1;
volatile int vx;

void expect(int got, int want)
{
    if (got != want)
        fails++;
}

/* A store through a pointer that names the global overrides the earlier store;
 * one through another object leaves it.
 */
int store_through(int *p)
{
    g = 1;
    *p = 2;
    return g;
}

void bump_g(void);

int across_call(void)
{
    g = 5;
    int before = g;
    bump_g();
    return before * 10 + g;
}
void bump_g(void)
{
    g += 1;
}

/* A narrow store changes part of a wider value. */
int partial(void)
{
    int x = 0x01020304;
    int *p = &x;
    *(char *) p = 0x7f;
    return *p;
}

int overlap_store(int *p)
{
    int a = *p;
    ((char *) p)[1] = 9;
    return a != *p;
}

/* The value after a join comes from whichever arm ran. */
int joined(int c)
{
    int x = 1;
    if (c)
        x = 2;
    g = x;
    return g + x;
}

int arm_store(int c, int *p)
{
    *p = 1;
    if (c)
        *p = 3;
    return *p;
}

/* A local whose address escapes can change under a call or a pointer store. */
void poke(int *p)
{
    *p = 42;
}

int escaped(void)
{
    int local = 1;
    int *p = &local;
    local = 2;
    *p = 3;
    int seen = local;
    poke(&local);
    return seen * 100 + local;
}

int overwritten(void)
{
    int local = 1;
    local = 2;
    local = 3;
    return local;
}

int dead_slot(int n)
{
    int unused[4];
    unused[0] = n;
    unused[1] = n + 1;
    return n * 2;
}

/* Two indexed stores to one array, read back in another order. */
int indexed(int i, int j)
{
    table[i] = 10;
    table[j] = 20;
    return table[i] * 100 + table[j];
}

int fields(struct pair *p)
{
    p->c = 7;
    p->b = 8;
    int sum = p->c + p->b;
    p->c = 9;
    return sum * 100 + p->c + p->b;
}

int loop_carry(int *p, int n)
{
    int sum = 0;
    *p = 1;
    for (int i = 0; i < n; i++) {
        sum += *p;
        *p = *p + 1;
    }
    return sum;
}

int volatile_pair(void)
{
    volatile int local = 1;
    local = 2;
    local = 3;
    return local;
}

/* The result of a comparison returned through a join of constants. */
int not_equal(int a, int b)
{
    return a != b;
}

int either(int a, int b, int c)
{
    return (a && b) || c;
}

int both(int a, int b)
{
    return a && b;
}

int in_range(int c)
{
    return c >= 'a' && c <= 'z';
}

/* Arithmetic the optimizer folds across steps. */
int chains(int x)
{
    int sum = 0;
    sum += ((x & 0xf0) & 0x3c);
    sum += ((x | 1) | 6);
    sum += ((x ^ 5) ^ 3);
    sum += ((x + 4) + 8);
    sum += ((x * 3) * 5);
    sum += ((x << 3) << 5);
    sum += ((x >> 3) >> 4);
    return sum;
}

unsigned unsigned_chains(unsigned x)
{
    return (((x >> 3) >> 4) + ((x << 1) << 2) + ((x & 0xff) & 0x0f));
}

int casts(int x)
{
    signed char c = (signed char) x;
    short s = (short) x;
    unsigned char uc = (unsigned char) x;
    unsigned short us = (unsigned short) x;
    int sum = (short) c + (int) (signed char) s + (int) (unsigned char) s;
    sum += (int) (unsigned char) c + (int) (unsigned short) uc;
    sum += (int) (short) uc + (int) (signed char) us + (int) us;
    return sum;
}

int below(unsigned x)
{
    return (x < 0u) * 4 + (x < x) * 2 + ((int) x < (int) x);
}

int offsets(char *p)
{
    char *q = p + 4;
    char *r = q + 8;
    char *s = r - 12;
    return r[0] * 10 + s[0] + (q + 3)[0];
}

/* Two bounds on one value reaching one block become a single unsigned test. */
int range_signed(int c)
{
    return c >= -5 && c <= 5;
}

int range_unsigned(unsigned c)
{
    return c >= 3u && c <= 0xfffffff0u;
}

int range_full(int c)
{
    return c >= -2147483647 - 1 && c <= 2147483647;
}

int range_empty(int c)
{
    return c >= 10 && c <= 5;
}

int range_outside(int c)
{
    return c < 3 || c > 9;
}

int main(void)
{
    int pair_values[5];
    struct pair p;
    char bytes[32];
    int values[3] = {1, 2, 3};

    expect(store_through(&g), 2);
    expect(store_through(&h), 1);
    expect(across_call(), 56);
    expect(partial(), 0x0102037f);
    pair_values[0] = 0x11223344;
    expect(overlap_store(&pair_values[0]), 1);
    expect(joined(0), 2);
    expect(joined(vone), 4);
    expect(arm_store(vzero, &g), 1);
    expect(arm_store(vone, &g), 3);
    expect(escaped(), 342);
    expect(overwritten(), 3);
    expect(dead_slot(vone), 2);
    expect(indexed(1, 2), 1020);
    expect(indexed(3, 3), 2020);
    expect(fields(&p), 1517 + 0);
    expect(loop_carry(&g, 4), 10);
    expect(volatile_pair(), 3);

    expect(not_equal(vone, vzero), 1);
    expect(not_equal(vone, vone), 0);
    expect(either(vzero, vone, vzero), 0);
    expect(either(vone, vone, vzero), 1);
    expect(either(vzero, vzero, vone), 1);
    expect(both(vone, vone), 1);
    expect(both(vone, vzero), 0);
    expect(in_range('a'), 1);
    expect(in_range('z'), 1);
    expect(in_range('a' - 1), 0);
    expect(in_range('z' + 1), 0);

    for (int i = 0; i < 4; i++) {
        vx = (i - 2) * 0x2345;
        int x = vx;
        int want = (x & 0x30) + (x | 7) + (x ^ 6) + (x + 12) + x * 15 +
                   (x << 8) + (x >> 7);
        expect(chains(x), want);
    }
    vx = 0x1ff;
    expect(casts(vx), 1528);
    vx = -3;
    expect(casts(vx), 66536);
    vx = 0x80;
    expect(casts(vx), 256);
    vx = 0x7f81;
    expect(casts(vx), 32776);
    expect(below(vone), 0);
    expect(unsigned_chains(0xfffffffe),
           (0xfffffffeu >> 7) + (0xfffffffeu << 3) + 0x0e);

    for (int i = 0; i < 32; i++)
        bytes[i] = i;
    expect(offsets(bytes), 127);
    expect(values[1] + values[2], 5);
    int probes[] = {-2147483647 - 1, -2147483647, -6, -5, -4, 0, 4, 5, 6, 9, 10,
                    2147483647};
    for (int i = 0; i < 12; i++) {
        int v = probes[i];
        unsigned u = (unsigned) v;
        expect(range_signed(v), (v >= -5) & (v <= 5));
        expect(range_unsigned(u), (u >= 3u) & (u <= 0xfffffff0u));
        expect(range_full(v), 1);
        expect(range_empty(v), 0);
        expect(range_outside(v), (v < 3) | (v > 9));
    }
    return fails;
}
