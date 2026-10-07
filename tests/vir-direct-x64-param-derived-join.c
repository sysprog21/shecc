int main(int argc, char **argv)
{
    int c = argv != 0;
    int x = argc + 5;
    int y;

    if (c)
        y = 2;
    else
        y = 3;
    return x + y;
}
