/*
 * Call arguments defined after a join the frontend resolves only once the whole
 * function has been read: an int, a comparison held in an int, a long long and
 * a pointer.
 */
static int g;

int take(int *p, int flag, long long wide, int n)
{
    return (p == &g) + flag * 2 + (int) (wide >> 32) * 4 + n * 16;
}

int deferred(int a, int b)
{
    if (!a || !b)
        return -1;
    int n = a * b;
    int flag = a < b;
    long long wide = (long long) a << 32;
    int *p = &g;
    return take(p, flag, wide, n);
}

int main()
{
    return deferred(1, 3) + deferred(0, 1) + 1;
}
