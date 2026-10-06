int a[64], b[64];
int dot(void)
{
    int s = 0;
    for (int i = 0; i < 64; i++)
        s += a[i] * b[i];
    return s;
}
void bump(void)
{
    for (int i = 0; i < 64; i++)
        a[i] = a[i] + 1;
}
int stride(void)
{
    int s = 0;
    for (int i = 0; i < 8; i++)
        s += a[i * 4];
    return s;
}
int dynamic_stride(int *p, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[i * 4];
    return s;
}
int unsigned_walk(int *p)
{
    int s = 0;
    for (unsigned i = 0; i < 64; i++)
        s += *p++;
    return s;
}
int unsigned_walk_start(int *p)
{
    int s = 0;
    for (unsigned i = 1; i < 8; i++)
        s += *p++;
    return s;
}
int unsigned_walk_index(int *p)
{
    int s = 0;
    for (unsigned i = 0; i < 8; i++)
        s += *p++ + i;
    return s;
}
int unsigned_walk_zero(int *p)
{
    int s = 0;
    for (unsigned i = 0xffffffffu; i < 8; i++)
        s += *p++;
    return s;
}
int unsigned_walk_volatile(volatile int *p)
{
    int s = 0;
    for (unsigned i = 0; i < 8; i++)
        s += *p++;
    return s;
}
int unsigned_reverse_sum(int *p, unsigned n)
{
    int s = 0;
    while (0u < n) {
        s += p[n - 1];
        n--;
    }
    return s;
}
int unsigned_reverse_volatile(volatile int *p, unsigned n)
{
    int s = 0;
    while (0u < n) {
        s += p[n - 1];
        n--;
    }
    return s;
}
int main(void)
{
    for (int i = 0; i < 64; i++) {
        a[i] = i;
        b[i] = 64 - i;
    }
    if (dot() != 43680)
        return 1;
    bump();
    if (stride() != 120 || dynamic_stride(a, 4) != 28)
        return 2;
    if (unsigned_walk(a) != 2080 || unsigned_walk_start(a) != 28)
        return 3;
    if (unsigned_walk_index(a) != 64 || unsigned_walk_zero(a))
        return 4;
    if (unsigned_walk_volatile(a) != 36)
        return 5;
    if (unsigned_reverse_sum(a, 0) || unsigned_reverse_sum(a, 1) != 1 ||
        unsigned_reverse_sum(a, 64) != 2080)
        return 6;
    return unsigned_reverse_volatile(a, 8) != 36;
}
