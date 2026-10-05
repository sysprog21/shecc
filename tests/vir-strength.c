int a[64], b[64];
int dot(void)
{
    int s = 0;
    for (int i = 0; i < 64; i++)
        s += a[i] * b[i];
    return s;
}
void bump(void)
{
    for (int i = 0; i < 64; i++)
        a[i] = a[i] + 1;
}
int stride(void)
{
    int s = 0;
    for (int i = 0; i < 8; i++)
        s += a[i * 4];
    return s;
}
int dynamic_stride(int *p, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[i * 4];
    return s;
}
int main(void)
{
    for (int i = 0; i < 64; i++) {
        a[i] = i;
        b[i] = 64 - i;
    }
    if (dot() != 43680)
        return 1;
    bump();
    return stride() != 120 || dynamic_stride(a, 4) != 28;
}
