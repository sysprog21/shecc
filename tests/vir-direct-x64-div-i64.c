int main(int argc, char **argv)
{
    long long signed_dividend = -0x7000000000000000LL + (long long) argc;
    long long signed_divisor = (long long) (argc + 2);
    long long signed_result =
        signed_dividend / signed_divisor + signed_dividend % signed_divisor;
    unsigned long long unsigned_dividend =
        0x8000000000000000ULL + (unsigned long long) argc;
    unsigned long long unsigned_result =
        unsigned_dividend / 7ULL + unsigned_dividend % 7ULL;

    (void) argv;
    return (int) (signed_result % 251LL +
                  (long long) (unsigned_result % 251ULL));
}
