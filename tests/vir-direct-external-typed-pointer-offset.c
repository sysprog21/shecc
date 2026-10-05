static int values[3] = {1, 2, 3};
extern int *wmemchr(const int *values, int value, unsigned long count);

int main(int argc, char **argv)
{
    int *match = wmemchr(values, 2, 3);
    int *copy = match;
    int index = argc - 1;

    (void) argv;
    return copy[index] - (index ? 3 : 2);
}
