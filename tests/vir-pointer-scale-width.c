/*
 * Exercise offsets within actual mapped storage, including a 32-bit index whose
 * byte offset exceeds 32 bits. Only a few pages are touched.
 */
#ifdef __SHECC__
#define RAW_SYSCALL __syscall
#else
extern long syscall(long number, ...);
#define RAW_SYSCALL syscall
#endif

int main(void)
{
    int values[8] = {0};
    int *middle = values + 4;
    volatile int negative = -3;
    middle[negative] = 7;
    if (values[1] != 7 || middle + negative != values + 1)
        return 1;
    unsigned char narrow = 255;
    unsigned short narrow_short = 257;
    int small[300] = {0};
    small[narrow] = 11;
    small[narrow_short] = 13;
    if (small[255] != 11 || small[257] != 13)
        return 5;
    volatile long long signed_negative = -2;
    middle[signed_negative] = 17;
    if (values[2] != 17)
        return 6;
    if (sizeof(void *) != 8)
        return 0;
#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__x86_64__)
    int mmap_number = 9, munmap_number = 11;
#elif defined(__aarch64__)
    int mmap_number = 222, munmap_number = 215;
#endif
    unsigned long long bytes = 20ULL << 30;
    long long mapped = RAW_SYSCALL(mmap_number, 0, bytes, 3, 0x4022, -1, 0);
    if (mapped < 0 && mapped >= -4095)
        return 80;
    int *base = (int *) mapped;
    volatile unsigned int index = (1u << 30) + 5u;
    int *high = base + index;
    int *expected = (int *) ((char *) base + ((unsigned long long) index * 4));
    base[index] = 37;
    if (&base[index] != expected)
        return 10;
    if (high != expected || *expected != 37)
        return 2;
    volatile unsigned int last = 0xffffffffu;
    high = base + last;
    expected = (int *) ((char *) base + ((unsigned long long) last * 4));
    base[last] = 41;
    base[last] += 2;
    if (base[last] != 43)
        return 11;
    base[last] -= 2;
    if (high != expected || *expected != 41)
        return 3;
    high = base + 0xffffffffu;
    if (high != expected || *high != 41)
        return 7;
    volatile unsigned int char_index = 0xffffffffu;
    char *chars = (char *) base;
    chars[char_index] = 23;
    if (*(chars + (unsigned long long) char_index) != 23)
        return 8;
    volatile int char_negative = -1;
    char *char_middle = chars + ((unsigned long long) char_index + 1);
    if (char_middle[char_negative] != 23)
        return 9;
    if (RAW_SYSCALL(munmap_number, mapped, bytes) != 0)
        return 4;
#endif
    return 0;
}
