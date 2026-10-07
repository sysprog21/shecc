static int values[2] = {11, 21};
extern int *wmemchr(const int *values, int value, unsigned long count);

int main(int argc, char **argv)
{
    int *pointer = wmemchr(values, 21, 2);
    int i = 0;
    int index = argc - 1;

    (void) argv;
    while (i < index) {
        pointer = values;
        i++;
    }
    return pointer[0] - (index ? 11 : 21);
}
