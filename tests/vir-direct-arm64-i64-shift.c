volatile unsigned long long source = 0x4000000000000001ULL;
volatile long long signed_source = -0x4000000000000000LL;
volatile unsigned long long expected_left = 0x8000000000000002ULL;
volatile unsigned long long expected_logical = 0x40000000ULL;
volatile long long expected_arithmetic = 0xffffffffc0000000LL;

int main(void)
{
    unsigned long long left = source << 1;
    unsigned long long logical = source >> 32;
    long long arithmetic = signed_source >> 32;

    return left != expected_left || logical != expected_logical ||
           arithmetic != expected_arithmetic;
}
