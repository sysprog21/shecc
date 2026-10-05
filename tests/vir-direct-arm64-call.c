volatile unsigned long long seed = 0xfedcba9876543210ULL;

unsigned long long helper(int value,
                          int b,
                          int c,
                          int d,
                          int e,
                          int f,
                          int g,
                          int h,
                          int i,
                          int j,
                          int k,
                          int l,
                          int m,
                          int n,
                          int o,
                          int p)
{
    return value + (unsigned int) p;
}

int main(void)
{
    unsigned long long value = seed;
    int result = helper(41, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16);

    return value == seed && result == 57 ? 0 : 1;
}
