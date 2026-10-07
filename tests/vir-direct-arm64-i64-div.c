volatile long long source = 0x300000003LL;
volatile long long divisor = 3;
volatile long long expected = 0x100000001LL;

int main(void)
{
    long long value = source / divisor;

    return value != expected;
}
