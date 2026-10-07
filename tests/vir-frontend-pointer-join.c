int main(int argc, char **argv)
{
    char *value;
    int byte;

    if (argc > 1)
        value = argv[1];
    else
        value = argv[0];
    byte = value[0];
    return argc > 1 ? byte : 47;
}
