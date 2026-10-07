int main(int argc, char **argv)
{
    return argc + (argv[argc] != (char *) 0) - argc;
}
