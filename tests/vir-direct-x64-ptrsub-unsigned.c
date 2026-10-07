int main(int argc, char **argv)
{
    int values[2];
    unsigned int index = argc;
    int *last = values + 1;
    int *first = last - index;

    *first = 29;
    return values[0];
}
