static unsigned int u32_zero(unsigned int x, unsigned int mask)
{
    unsigned int count = 0;
    while ((x & mask) == 0 && count < 40) {
        x = x * 1664525u + 1013904223u;
        count++;
    }
    return x ^ count;
}

static unsigned int u32_nonzero(unsigned int x, unsigned int mask)
{
    unsigned int count = 0;
    while ((x & mask) != 0 && count < 40) {
        x = x * 1664525u + 1013904223u;
        count++;
    }
    return x ^ count;
}

static int s32_zero(int x, int mask)
{
    unsigned int count = 0;
    while ((x & mask) == 0 && count < 40) {
        x = (int) ((unsigned int) x * 1664525u + 1013904223u);
        count++;
    }
    return x ^ count;
}

static int s32_nonzero(int x, int mask)
{
    unsigned int count = 0;
    while ((x & mask) != 0 && count < 40) {
        x = (int) ((unsigned int) x * 1664525u + 1013904223u);
        count++;
    }
    return x ^ count;
}

static unsigned long long u64_zero(unsigned long long x,
                                   unsigned long long mask)
{
    unsigned int count = 0;
    while ((x & mask) == 0 && count < 40) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        count++;
    }
    return x ^ count;
}

static unsigned long long u64_nonzero(unsigned long long x,
                                      unsigned long long mask)
{
    unsigned int count = 0;
    while ((x & mask) != 0 && count < 40) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        count++;
    }
    return x ^ count;
}

int main(void)
{
    unsigned int sum = 1;
    for (unsigned int i = 0; i < 1000; i++) {
        unsigned int mask = 1u << (i % 32);
        unsigned long long wide_mask = 1ULL << (i % 64);
        unsigned long long wide = i * 0x100000001ULL + 7;
        int signed_value = (int) (i * 31337u);
        sum = sum * 33u + u32_zero(i, mask);
        sum = sum * 33u + u32_nonzero(i, mask);
        sum = sum * 33u + (unsigned int) s32_zero(signed_value, (int) mask);
        sum = sum * 33u + (unsigned int) s32_nonzero(signed_value, (int) mask);
        sum = sum * 33u + (unsigned int) u64_zero(wide, wide_mask);
        sum = sum * 33u + (unsigned int) u64_nonzero(wide, wide_mask);
    }
    return sum != 1983674465u;
}
