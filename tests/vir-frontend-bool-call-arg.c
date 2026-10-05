/*
 * Truth values passed to a direct call: a _Bool known at once, one resolved
 * after a join, and a comparison passed as is.
 */
int take(_Bool b, int k)
{
    return b * 10 + k;
}

int direct(int a, int b)
{
    _Bool f = a < b;

    return take(f, 1);
}

int deferred(int a, int b)
{
    if (!a || !b)
        return 0;
    _Bool f = a < b;

    return take(f, 2);
}

int cmp_arg(int a, int b)
{
    return take(a < b, 3);
}

int main()
{
    return direct(1, 2) + deferred(1, 2) * 2 + cmp_arg(2, 1) * 4;
}
