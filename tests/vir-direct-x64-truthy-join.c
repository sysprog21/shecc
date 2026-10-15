int main(int argc)
{
    int x;

    if (argc - 1)
        x = 1;
    else
        x = 0;
    if (x)
        return 31;
    return 0;
}
