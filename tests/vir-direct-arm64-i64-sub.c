volatile long long source = 0x200000000LL;
volatile long long expected = 0x1ffffffffLL;

int main(int argc)
{
    long long value = source - argc;

    return value == expected ? 0 : 1;
}
