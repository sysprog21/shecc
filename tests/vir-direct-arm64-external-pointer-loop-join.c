static int values[3] = {11, 21, 0};
extern int *wmemchr(const int *values, int value, unsigned long count);
extern int *wcschr(const int *values, int value);

int main(int argc, char **argv)
{
    int *pointer = wmemchr(values, 11, 2);
    int *other = wcschr(values, 11);
    int *saved;
    int i = 0;
    int index = argc - 1;

    (void) argv;
    while (i < index) {
        saved = pointer;
        pointer = other + 1;
        other = saved;
        i++;
    }
    return pointer[0] - (index ? 21 : 11);
}
