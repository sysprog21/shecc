static long long seventh(long long a,
                         long long b,
                         long long c,
                         long long d,
                         long long e,
                         long long f,
                         long long g)
{
    return g + 0LL;
}

int main(void)
{
    return (int) (seventh(1LL, 2LL, 3LL, 4LL, 5LL, 6LL, 0x100000000LL) >> 32);
}
