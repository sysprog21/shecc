int main(int argc)
{
    int a[2];
    int *p = a + 1;

    if (argc == 1)
        *p = 40;
    else
        *p = 41;
    return *p + 2;
}
