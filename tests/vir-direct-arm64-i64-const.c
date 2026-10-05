volatile unsigned long long expected = 0xfedcba9876543210ULL;

int main(void)
{
    return expected == 0xfedcba9876543210ULL ? 0 : 1;
}
