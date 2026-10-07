volatile unsigned long long seed = 0x100000001ULL;

int main(int argc)
{
    unsigned long long base = seed;
    unsigned long long a = base + argc + 1;
    unsigned long long b = base + argc + 2;
    unsigned long long c = base + argc + 3;
    unsigned long long d = base + argc + 4;
    unsigned long long e = base + argc + 5;
    unsigned long long f = base + argc + 6;
    unsigned long long g = base + argc + 7;
    unsigned long long h = base + argc + 8;
    unsigned long long sum = a + b + c + d + e + f + g + h;
    unsigned long long expected = 0x800000034ULL;

    return sum == expected ? 0 : 1;
}
