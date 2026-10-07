int seventh_pointer(int a, int b, int c, int d, int e, int f, char **values)
{
    return values[0] != (char *) 0 ? a + b + c + d + e + f : 0;
}

int main(int argc, char **argv)
{
    return seventh_pointer(1, 2, 3, 4, 5, 6, argv) + argc;
}
