int main(int argc, char **argv)
{
    char *first = argv[0];
    int value = 0;
    int *slot = &value;

    *slot = argc;
    return first[1];
}
