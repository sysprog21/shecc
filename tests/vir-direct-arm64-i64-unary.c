volatile unsigned long long source = 0x100000000ULL;
volatile unsigned long long expected_neg = 0xffffffff00000000ULL;
volatile unsigned long long expected_not = 0xfffffffeffffffffULL;

int main(void)
{
    unsigned long long neg_value = -source;
    unsigned long long not_value = ~source;

    return neg_value != expected_neg || not_value != expected_not;
}
