/*
 * Resident call arguments, pairs and narrow stack arguments retain their bits.
 */
static unsigned long long mix(signed char a,
                              unsigned char b,
                              short c,
                              unsigned short d,
                              unsigned int e,
                              long long f,
                              signed char g,
                              unsigned short h)
{
    return (unsigned long long) a + 3ull * b + 5ull * c + 7ull * d + 11ull * e +
           13ull * f + 17ull * g + 19ull * h;
}
static unsigned long long swap(unsigned long long a, unsigned long long b)
{
    return a * 3 + b * 7;
}
int main(void)
{
    unsigned long long (*call)(signed char, unsigned char, short,
                               unsigned short, unsigned int, long long,
                               signed char, unsigned short) = mix;
    unsigned long long expected = 18446728068328672961ull;
    for (unsigned int n = 0; n < 20; n++) {
        unsigned long long live = 0x123456789abcdef0ull + n;
        unsigned long long result = call(-7, 241, -1234, 65000, 4000000001u,
                                         -1234567890123ll, -99, 65530);
        if (result != expected || live != 0x123456789abcdef0ull + n)
            return 1;
        if (swap(live, result) != 3935193364725220375ull + 3ull * n)
            return 2;
    }
    return 0;
}
