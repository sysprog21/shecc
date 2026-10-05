volatile unsigned long long expected = 0xffffffff00000000ULL;

int main(void)
{
    return expected == 0xffffffff00000000ULL ? 0 : 1;
}
