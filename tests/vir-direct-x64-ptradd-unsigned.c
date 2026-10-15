int main(int argc, char **argv)
{
    char *p = argv[(unsigned int) argc & 1U];

    return p != (char *) 0 ? 29 : argc - argc;
}
