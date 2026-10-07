int main(void)
{
    int a[4200];
    int *p = a + 4100;

    *p = 7;
    return a[4100] - 7;
}
