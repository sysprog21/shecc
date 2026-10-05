int main(int argc, char **argv)
{
    char **p;

    if (argc > 1)
        p = argv;
    else
        p = (char **) 0;
    if (p != (char **) 0)
        return 29;
    return 0;
}
