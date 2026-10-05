int main(int argc)
{
    long long first = 0;
    long long second = 0;
    long long *p = &first;
    long long *q = &second;

    if (argc > 1)
        q = p;
    *q = 0x100000000LL + (long long) argc;
    return (int) (first >> 32) + 2 * (int) (second >> 32);
}
