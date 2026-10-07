static short combine(signed char a,
                     unsigned char b,
                     short c,
                     unsigned short d,
                     signed char e,
                     unsigned char f,
                     short g,
                     unsigned short h)
{
    if (a != -5 || b != 250 || c != -20 || d != 40000 || e != 2 || f != 3 ||
        g != 4 || h != 50000)
        return 0;
    return 37;
}

static signed char negative(void)
{
    return -1;
}

static unsigned short maximum(void)
{
    return 65535;
}

int main(void)
{
    return combine(-5, 250, -20, 40000, 2, 3, 4, 50000) + negative() +
           (maximum() == 65535);
}
