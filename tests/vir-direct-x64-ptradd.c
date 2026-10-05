int main(int argc, char **argv)
{
    int i = argc & 1;
    char *p = argv[i];

    return p != (char *) 0 ? 29 : argc - argc;
}
