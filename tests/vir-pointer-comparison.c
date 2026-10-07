int main(void)
{
    if (sizeof(void *) == 8) {
        int *pointer = (int *) 0x100000000ULL;
        if (0 == pointer || pointer == 0)
            return 1;
        if (!(0 != pointer) || !(pointer != 0))
            return 2;
    }
    return 0;
}
