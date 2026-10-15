int main(int argc)
{
    int n = -argc;

    return ((n >> argc) < 0) * 10 +
           (((unsigned) n >> argc) == (0x7fffffffU >> (argc - 1))) +
           ((~argc) == n - 1) + ((argc << 2) == argc * 4);
}
