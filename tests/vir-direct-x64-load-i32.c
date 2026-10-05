int main(int argc, char **argv)
{
    int *p = (int *) argv;

    return *p == *p ? 29 : argc - argc;
}
