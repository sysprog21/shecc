int present(char **values);

int main(int argc, char **argv)
{
    return present(argv) + argc;
}
int present(char **values)
{
    return values[0] != (char *) 0 ? 29 : 0;
}
