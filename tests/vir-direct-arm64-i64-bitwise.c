volatile unsigned long long source = 0xf0f0f0f0f0f0f0f0ULL;
volatile unsigned long long mask = 0x0ff00ff00ff00ff0ULL;
volatile unsigned long long expected_and = 0x00f000f000f000f0ULL;
volatile unsigned long long expected_or = 0xfff0fff0fff0fff0ULL;
volatile unsigned long long expected_xor = 0xff00ff00ff00ff00ULL;

int main(void)
{
    unsigned long long and_value = source & mask;
    unsigned long long or_value = source | mask;
    unsigned long long xor_value = source ^ mask;

    return and_value != expected_and || or_value != expected_or ||
           xor_value != expected_xor;
}
