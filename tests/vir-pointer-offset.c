volatile long long wide_index;
int object = 73;
long long address_bits(char *base, long long offset)
{
    return (long long) (base + offset);
}
int read_scaled(char *base, long long index)
{
    return *(int *) (base + (index << 2));
}
int main(void)
{
    if (sizeof(void *) != 8)
        return 0;
    if (address_bits((char *) 5, 0x100000000LL) != 0x100000005LL ||
        (unsigned long long) address_bits((char *) 5, -0x100000001LL) !=
            0xffffffff00000004ULL)
        return 1;
    wide_index = 0x100000001LL;
    char *base = (char *) ((unsigned long long) &object - 0x400000004ULL);
    if (read_scaled(base, wide_index) != 73)
        return 2;
    wide_index = -0x100000001LL;
    base = (char *) ((unsigned long long) &object + 0x400000004ULL);
    return read_scaled(base, wide_index) != 73;
}
