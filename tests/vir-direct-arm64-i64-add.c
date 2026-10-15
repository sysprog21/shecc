volatile long long source = 0xffffffffLL;
volatile long long expected = 0x100000000LL;

int main(int argc)
{
    long long value = source + argc;

    return value == expected ? 0 : 1;
}
