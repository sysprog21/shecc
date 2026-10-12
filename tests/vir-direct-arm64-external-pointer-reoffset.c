static int values[3] = {1, 2, 3};
extern void *malloc(unsigned long size);
extern int *wmemchr(const int *values, int value, unsigned long count);

int main(int argc, char **argv)
{
    int *pointer = malloc(3 * sizeof(*pointer));
    int index = argc - 1;

    (void) argv;
    pointer += index;
    pointer = wmemchr(values, 2, 3);
    pointer += index;
    return pointer[0] - (index ? 3 : 2);
}
