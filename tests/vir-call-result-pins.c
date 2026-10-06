static unsigned long long wide(int n)
{
    return 0x123456789abcdef0ULL + (unsigned long long) n * 0x100000001ULL;
}

static unsigned long long combine(unsigned long long a, unsigned long long b)
{
    return a * 3ULL ^ (b + 17ULL);
}

static signed char narrow(int n)
{
    return n - 130;
}

static _Bool truth(int n)
{
    return n & 1;
}

static short narrow16(int n)
{
    return 30000 - n;
}

static unsigned int step(unsigned int x)
{
    return x * 1664525u + 1013904223u;
}

int main(void)
{
    unsigned long long (*volatile indirect)(int) = wide;
    unsigned long long a = wide(7);
    unsigned long long b = wide(13);
    unsigned long long c = combine(a, b);
    unsigned long long d = indirect(19);
    signed char x = narrow(5);
    signed char y = narrow(9);
    _Bool t = truth(3);
    _Bool f = truth(4);
    short u = narrow16(50000);
    short v = narrow16(60000);
    unsigned int state = 7;
    for (int i = 0; i < 32; i++)
        state = step(state);
    return a != 1311768497528561399ULL || b != 1311768523298365181ULL ||
           c != 2641737194205234155ULL || d != 1311768549068168963ULL ||
           x != -125 || y != -121 || !t || f || u != -20000 || v != -30000 ||
           state != 463890471u || a + b + c + d != 6577042764100329698ULL;
}
