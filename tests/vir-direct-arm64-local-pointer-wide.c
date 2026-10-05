int main(int argc)
{
    long long value = 0x200000001LL;
    long long *p = &value;
    long long **pp = &p;

    **pp = (long long) argc + 0x400000000LL;
    return (int) (**pp >> 32);
}
