static int present(char **values)
{
    return values[0] != (char *) 0 ? 29 : 0;
}

int main(int argc, char **argv)
{
    return present(argv) + argc;
}
