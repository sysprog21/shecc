/*
 * A value deferred past a join keeps its place in the block. A later read of a
 * variable whose write was deferred waits for it; a variable written again
 * after a deferral reads or writes it makes the function fall back.
 */
int g;

void h_g(int x)
{
    g = x;
}

int h_mix(int i, int u, int v)
{
    return i * 100 + u * 10 + v;
}

int reread(int c)
{
    if (!c || c > 5)
        return 9;
    int r = 0;
    r = c + 1;
    return r;
}

int rebind_bool(int c, int a)
{
    if (!c || !a)
        return 1;
    _Bool r = a;
    a = 0;
    return r * 10 + a;
}

int rebind_arg(int c, int a)
{
    int x = 1;
    if (c && a)
        x = 2;
    h_g(x);
    x = 7;
    return x;
}

int rebind_loop(int n)
{
    int s = 0, u = 1, v = 2;
    for (int i = 0; i < n; i++) {
        s += h_mix(i, u, v);
        u = v + 1;
        v = u + i;
    }
    return s;
}

int main()
{
    int a = reread(1);
    int b = rebind_bool(1, 5);
    int c = rebind_arg(1, 1);
    int d = rebind_loop(4);

    return a + b + c * 2 + g * 3 + (d == 758) * 20;
}
