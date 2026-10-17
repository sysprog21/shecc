char *identity(char *value);

int main(int argc, char **argv)
{
    char *original = argv[0];
    char *value = identity(original);

    return value == original ? 29 : argc - argc;
}
char *identity(char *value)
{
    return value;
}
