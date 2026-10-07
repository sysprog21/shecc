/*
 * A comparison stored in a _Bool and returned, after a join the frontend
 * resolves only once the whole function has been read.
 */
_Bool not_six(int a, int b)
{
    if (!a || !b)
        return 0;
    int n = a * b;
    _Bool r = n != 6;
    return r;
}

int main()
{
    return not_six(2, 3) + not_six(2, 4) * 2 + not_six(0, 1) * 4;
}
