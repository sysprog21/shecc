int main(void)
{
    int a = -10, b = 2;
    unsigned int c = a / b;
    unsigned int u1 = 4000000000U, u2 = 2U;
    int s = u1 / u2;
    short x = -1;
    unsigned int y = 10;
    unsigned int l = x + y, r = y + x;
    short n = -5;
    unsigned int limit = 70000U;
    if (c != 4294967291U)
        return 1;
    if (s != 2000000000)
        return 2;
    if (l != 9 || r != 9)
        return 3;
    if (n < limit)
        return 4;
    return 0;
}
