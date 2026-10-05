static int values[2] = {11, 21};
extern int *wmemchr(const int *values, int value, unsigned long count);

int main(int argc, char **argv)
{
    int *pointer;

    (void) argv;
    if (argc > 1)
        pointer = wmemchr(values, 11, 2);
    else
        pointer = &values[1];
    return *pointer - (argc > 1 ? 11 : 21);
}
