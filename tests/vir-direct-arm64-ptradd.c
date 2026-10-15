int main(int argc)
{
    int a[2];
    int *p = a + 1;

    *p = argc + 40;
    return a[1] + 2;
}
