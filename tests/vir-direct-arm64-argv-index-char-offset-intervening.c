int main(int argc, char **argv)
{
    char *value = argv[1];
    volatile int saved = 0;
    volatile int *slot = &saved;

    *slot = argc;
    return value[1];
}
