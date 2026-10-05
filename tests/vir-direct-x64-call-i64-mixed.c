static long long
mix(long long a, int b, long long c, int d, long long e, int f, long long g)
{
    return a + b + c + d + e + f + g;
}

int main(void)
{
    return (int) (mix(0x100000000LL, 1, 0x100000000LL, 2, 0x100000000LL, 3,
                      0x100000000LL) >>
                  32);
}
