volatile unsigned long long source = 0x200000001ULL;
volatile unsigned long long expected_add = 0x200000008ULL;
volatile unsigned long long expected_sub = 0x1fffffffaULL;

int main(void)
{
    unsigned long long add = source + 7;
    unsigned long long add_left = 7 + source;
    unsigned long long sub = source - 7;
    unsigned long long add_shift = source + 0x1000;
    unsigned long long sub_shift = source - 0x1000;

    return add != expected_add || add_left != expected_add ||
           sub != expected_sub || add_shift != 0x200001001ULL ||
           sub_shift != 0x1fffff001ULL;
}
