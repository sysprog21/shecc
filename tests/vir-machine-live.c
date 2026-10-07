/* Constants and branch results remain live through allocated edge copies. */
static unsigned int tick(unsigned int x)
{
    return x * 1664525u + 1013904223u;
}
int main(void)
{
    unsigned int x = 1, sum = 0;
    for (unsigned int i = 0; i < 1024; i++) {
        x = tick(x);
        if (x & 8)
            sum += x ^ i;
        else
            sum -= x + i;
    }
    if (sum != 1478761984u)
        return 1;
    long long a = 0x123456789LL, b = 0x345678901LL;
    for (int i = 0; i < 17; i++) {
        long long old = a;
        a = b;
        b = old;
        x = tick(x);
    }
    return a != 0x345678901LL || b != 0x123456789LL;
}
