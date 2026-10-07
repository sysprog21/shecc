int helper(long long a,
           long long b,
           long long c,
           long long d,
           long long e,
           long long f,
           long long g,
           long long h)
{
    return a == 0x100000001LL && b == 0x200000002LL && c == 0x300000003LL &&
           d == 0x400000004LL && e == 0x500000005LL && f == 0x600000006LL &&
           g == 0x700000007LL && h == 0x800000008LL;
}

int main(void)
{
    return helper(0x100000001LL, 0x200000002LL, 0x300000003LL, 0x400000004LL,
                  0x500000005LL, 0x600000006LL, 0x700000007LL, 0x800000008LL) *
           42;
}
