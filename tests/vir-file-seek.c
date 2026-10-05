#include <stdio.h>

int main(void)
{
    FILE *file = fopen("tests/vir-file-seek.c", "r");
    if (!file || ftell(file) != 0 || fseek(file, 5, SEEK_SET) ||
        ftell(file) != 5 || fseek(file, -2, SEEK_CUR) || ftell(file) != 3 ||
        fseek(file, 0, SEEK_END) || ftell(file) <= 5)
        return 1;
    int end = ftell(file);
    if (!fseek(file, -100, SEEK_SET) || ftell(file) != end ||
        !fseek(file, 0, -3) || ftell(file) != end)
        return 2;
#if defined(__x86_64__) || defined(__aarch64__)
    /* A valid position must not be mistaken for a truncated -1 result. */
    if (fseek(file, 2147483647, SEEK_SET) ||
        fseek(file, 2147483647, SEEK_CUR) || fseek(file, 1, SEEK_CUR))
        return 3;
#endif
    fclose(file);
    return 0;
}
