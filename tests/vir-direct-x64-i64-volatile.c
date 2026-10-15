int main(int argc)
{
    volatile long long wide = 0;
    volatile long long *p = &wide;

    *p = 0x100000000LL + (long long) argc;
    return (int) (*p >> 32);
}
