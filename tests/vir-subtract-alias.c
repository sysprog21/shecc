/* Phi affinity can reuse the subtrahend's register for the loop result. */
int subtract_loop(int seed, int count)
{
    int sum = seed;
    for (int index = 0; index < count; index++)
        sum = index - sum;
    return sum;
}

long long subtract_wide_loop(long long seed, int count)
{
    long long sum = seed;
    for (int index = 0; index < count; index++)
        sum = 4294967311LL + index - sum;
    return sum;
}

int main(void)
{
    if (subtract_loop(7, 100001) != 49993)
        return 1;
    if (subtract_loop(7, 100000) != 50007)
        return 2;
    if (subtract_wide_loop(-8589934600LL, 100001) != 12884951911LL)
        return 3;
    if (subtract_wide_loop(-8589934600LL, 100000) != -8589884600LL)
        return 4;
    return 0;
}
