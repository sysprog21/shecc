int main(int argc)
{
    long long value = 0;
    long long *p = &value;

    *p = 0x100000000LL + (long long) argc;
    return (int) (*p >> 32);
}
