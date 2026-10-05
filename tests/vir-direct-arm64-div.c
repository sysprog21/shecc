int main(int argc)
{
    int signed_value = argc - 5;
    unsigned int unsigned_value = (unsigned int) argc + 100;
    long long signed_wide = (long long) (argc - 5);
    unsigned long long unsigned_wide = (unsigned long long) argc + 100;
    long long mixed_signed = (long long) argc - 10;
    unsigned int mixed_unsigned = (unsigned int) argc + 1;

    return signed_value / 2 + (int) (unsigned_value / 100) +
           (int) (signed_wide / 2) + (int) (unsigned_wide / 100) +
           signed_value % 3 + (int) (unsigned_value % 7) +
           (int) (signed_wide % 3) + (int) (unsigned_wide % 7) +
           (int) (mixed_signed / mixed_unsigned) +
           (int) (mixed_signed % mixed_unsigned) + 50;
}
