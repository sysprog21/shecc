static int shift_and_use(int a, int b, int c, int d)
{
    return (a << b) - d;
}

int main(int argc, char **argv)
{
    return shift_and_use(2, 1, 1, 10);
}
