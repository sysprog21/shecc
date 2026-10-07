static int values[3] = {11, 21, 0};
extern int *wmemchr(const int *values, int value, unsigned long count);
extern int *wcschr(const int *values, int value);

int main(int argc, char **argv)
{
    int *pointer;
    int index = argc - 1;

    (void) argv;
    if (argc > 1)
        pointer = wmemchr(values, 11, 2);
    else
        pointer = wcschr(values, 11);
    return pointer[index] - (index ? 21 : 11);
}
