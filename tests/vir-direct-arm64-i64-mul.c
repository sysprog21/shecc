volatile unsigned long long source = 0x100000001ULL;
volatile unsigned long long factor = 3;
volatile unsigned long long expected = 0x300000003ULL;

int main(void)
{
    unsigned long long value = source * factor;

    return value != expected ? 1 : 0;
}
