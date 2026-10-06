static int
sum10(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j)
{
    return a + b + c + d + e + f + g + h + i + j;
}
static int frame(int n)
{
    volatile unsigned char bytes[5000];
    bytes[0] = n;
    bytes[4999] = n + 1;
    return bytes[0] + bytes[4999] + sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
}
static int recursive(int n)
{
    volatile int saved[8];
    saved[0] = n;
    saved[7] = n + 1;
    return n ? recursive(n - 1) + saved[0] + saved[7] : saved[7];
}
static int live_condition(int a, int b)
{
    int c = a < b;
    if (c)
        return c + frame(a);
    return c;
}
int main(void)
{
    return frame(7) != 70 || recursive(7) != 64 || live_condition(7, 8) != 71 ||
           live_condition(8, 7) != 0;
}
