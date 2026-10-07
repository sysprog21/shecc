volatile unsigned int unsigned_source = 0xffffffffU;
volatile int signed_source = -1;
volatile unsigned long long expected_unsigned = 0xffffffffULL;
volatile long long expected_signed = -1LL;

int main(void)
{
    unsigned long long unsigned_value = (unsigned long long) unsigned_source;
    long long signed_value = (long long) signed_source;

    return unsigned_value != expected_unsigned ||
           signed_value != expected_signed;
}
