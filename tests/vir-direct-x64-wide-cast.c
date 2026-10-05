int main(int argc)
{
    long long x = (long long) argc;

    return (int) x == argc ? (argc == 1 ? 27 : 5) : 99;
}
