volatile unsigned long long expected = 0x100000000ULL;

int main(void)
{
    return expected == 0x100000000ULL ? 0 : 1;
}
