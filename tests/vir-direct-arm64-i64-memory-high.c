volatile long long source = 0x100000001LL;
volatile long long expected = 0x200000001LL;

int main(void)
{
    volatile long long value = 0;
    volatile long long *pointer = &value;

    *pointer = source;
    return *pointer == expected ? 1 : 0;
}
