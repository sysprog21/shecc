int main(int argc, char **argv)
{
    char *p = argv[0];
    int value = 0;
    int *slot = &value;

    *slot = argc;
    return *p;
}
