int main(int argc, char **argv)
{
    unsigned int value = 1000U * argc;
    unsigned int high_bit = 0x80000000U + (unsigned int) argc;

    (void) argv;
    return value / 7U + value % 7U + high_bit / 3U + high_bit % 3U;
}
