static int values[3] = {11, 21, 0};
extern int *wmemchr(const int *values, int value, unsigned long count);
extern int *wcschr(const int *values, int value);

int main(int argc, char **argv)
{
    int *pointer;

    (void) argv;
    if (argc > 1) {
        int *match = wmemchr(values, 11, 2);

        pointer = match;
    } else {
        int *match = wcschr(values, 21);

        pointer = match;
    }
    return *pointer - (argc > 1 ? 11 : 21);
}
