int main(int argc, char **argv)
{
    if (argc < 2)
        return 0;
    argv[1] = argv[0];
    return argv[1] == argv[0] ? 29 : 0;
}
