/* Alternate incoming values and overlapping old/new values must not alias. */
static unsigned int alternatives(int n)
{
    unsigned int a = 1, b = 3, c = 7;
    for (int i = 0; i < n; i++) {
        unsigned int a1 = b + 1, a2 = c + 2;
        if (i & 1)
            a = a1;
        else
            a = a2;
        b += a;
        c ^= a + i;
    }
    return a + b + c;
}
static unsigned long long mix(unsigned long long x)
{
    return x * 3 + 7;
}
static unsigned long long overlapping(unsigned int n)
{
    unsigned long long a = 3, b = 7, s = 0;
    for (unsigned int i = 0; i < n; i++) {
        unsigned long long next = a * 5 + i;
        s += a + next;
        if (i & 1)
            b = mix(next) ^ b;
        else
            b += next;
        a = next;
    }
    return a + b + s;
}
int main(void)
{
    unsigned int narrow = 0;
    unsigned long long wide = 0;
    for (int n = 0; n < 100; n++) {
        narrow = narrow * 33u + alternatives(n);
        wide = wide * 33 + overlapping(n);
    }
    return narrow != 1813477397u || wide != 7931923482355807455ULL;
}
